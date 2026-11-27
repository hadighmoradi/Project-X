// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/control_flow.hpp"
#include "xfrontend/parser.hpp"
#include "xfrontend/semantic.hpp"
#include "xfrontend/x64_codegen.hpp"
#include "test_report.hpp"

#include <algorithm>
#include <chrono>
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

CompilationUnit parse_unit(const std::string& source) {
    auto parsed = parse_source(source);
    require(parsed.succeeded(), parsed.diagnostics.empty()
        ? "B13-B source parse failed without a diagnostic"
        : "B13-B source parse failed: " + parsed.diagnostics.front().code + " " +
            parsed.diagnostics.front().message);
    return std::move(parsed.unit);
}

struct CompiledFixture {
    CompilationUnit unit;
    ControlFlowResult flow;
    X64CodegenResult codegen;
    std::vector<std::uint8_t> code;
};

CompiledFixture compile_fixture(const std::string& source,
                                X64CodegenOptions options = X64CodegenOptions{
                                    "n114_b13b_expression_anchor", make_coff_x86_64_object_target()}) {
    CompiledFixture fixture;
    fixture.unit = parse_unit(source);
    fixture.flow = analyze_control_flow(fixture.unit);
    require(fixture.flow.succeeded(), fixture.flow.diagnostics.empty()
        ? "B13-B Flow analysis failed without a diagnostic"
        : "B13-B Flow analysis failed: " + fixture.flow.diagnostics.front().code + " " +
            fixture.flow.diagnostics.front().message);
    fixture.codegen = compile_x64_coff_object(fixture.unit, std::move(options));
    require(fixture.codegen.succeeded(), fixture.codegen.diagnostics.empty()
        ? "B13-B native code generation failed without a diagnostic"
        : "B13-B native code generation failed: " + fixture.codegen.diagnostics.front().code + " " +
            fixture.codegen.diagnostics.front().message);
    const auto text = std::find_if(fixture.codegen.native_object.object_file.sections.begin(),
        fixture.codegen.native_object.object_file.sections.end(), [](const ObjectSection& section) {
            return section.kind == ObjectSectionKind::Text;
        });
    require(text != fixture.codegen.native_object.object_file.sections.end(),
        "B13-B object has no function text section");
    fixture.code = text->contents;
    return fixture;
}

const ControlFlowGraph& first_graph(const CompiledFixture& fixture) {
    require(!fixture.flow.functions.empty(), "B13-B Flow result has no function graph");
    return fixture.flow.functions.front();
}

std::vector<X64ExpressionBranchAnchor> anchors_for_function(
    const std::vector<X64ExpressionBranchAnchor>& anchors, std::size_t function_index = 0U) {
    std::vector<X64ExpressionBranchAnchor> selected;
    for (const auto& anchor : anchors) if (anchor.function_index == function_index) selected.push_back(anchor);
    return selected;
}

void validate_fixture(const CompiledFixture& fixture) {
    const auto& graph = first_graph(fixture);
    const auto anchors = anchors_for_function(fixture.codegen.flow_expression_branch_anchors, 0U);
    const auto validation = validate_x64_expression_branch_anchors(
        fixture.code, graph.expression_controls, graph.edges, anchors, 0U);
    require(validation.succeeded, validation.diagnostics.empty()
        ? "B13-B expression-anchor validator failed without a diagnostic"
        : "B13-B expression-anchor validator failed: " + validation.diagnostics.front());
}

std::vector<X64ExpressionEdgeOccupancyCapture> occupancy_for_function(
    const std::vector<X64ExpressionEdgeOccupancyCapture>& captures, std::size_t function_index = 0U) {
    std::vector<X64ExpressionEdgeOccupancyCapture> selected;
    for (const auto& capture : captures) if (capture.function_index == function_index) selected.push_back(capture);
    return selected;
}

const X64ExpressionEdgeOccupancyCapture& find_occupancy(
    const std::vector<X64ExpressionEdgeOccupancyCapture>& captures,
    FlowExpressionId expression_id, FlowEdgeKind kind) {
    const auto found = std::find_if(captures.begin(), captures.end(), [&](
        const X64ExpressionEdgeOccupancyCapture& capture) {
        return capture.expression_id == expression_id && capture.edge_kind == kind;
    });
    require(found != captures.end(), "B13-B expected Expression occupancy capture was not found");
    return *found;
}

void validate_occupancy_fixture(const CompiledFixture& fixture) {
    const auto& graph = first_graph(fixture);
    const auto anchors = anchors_for_function(fixture.codegen.flow_expression_branch_anchors, 0U);
    const auto captures = occupancy_for_function(fixture.codegen.flow_expression_edge_occupancy_captures, 0U);
    const auto validation = validate_x64_expression_edge_occupancy_captures(
        fixture.code, graph.expression_controls, graph.edges, anchors, captures, 0U);
    require(validation.succeeded, validation.diagnostics.empty()
        ? "B13-B Expression occupancy validator failed without a diagnostic"
        : "B13-B Expression occupancy validator failed: " + validation.diagnostics.front());
}

const X64ExpressionBranchAnchor& find_anchor(
    const std::vector<X64ExpressionBranchAnchor>& anchors,
    FlowExpressionId expression_id, FlowEdgeKind kind) {
    const auto found = std::find_if(anchors.begin(), anchors.end(), [&](const X64ExpressionBranchAnchor& anchor) {
        return anchor.expression_id == expression_id && anchor.edge_kind == kind;
    });
    require(found != anchors.end(), "B13-B expected expression edge anchor was not found");
    return *found;
}

void test_c01_and_branch_has_exact_four_machine_anchors() {
    const auto fixture = compile_fixture(
        "module N114C01; public function Main(bool flag, bool rhs) returns bool { return flag && rhs; }");
    const auto& graph = first_graph(fixture);
    require(graph.expression_controls.size() == 1U, "C01 expected exactly one logical expression control");
    const auto anchors = anchors_for_function(fixture.codegen.flow_expression_branch_anchors);
    require(anchors.size() == 4U, "C01 expected exactly four expression branch anchors");
    validate_fixture(fixture);
    const auto id = graph.expression_controls.front().id;
    const auto& evaluate = find_anchor(anchors, id, FlowEdgeKind::ExpressionEvaluateRhs);
    const auto& short_edge = find_anchor(anchors, id, FlowEdgeKind::ExpressionShortCircuit);
    const auto& rhs_merge = find_anchor(anchors, id, FlowEdgeKind::ExpressionRhsMerge);
    const auto& short_merge = find_anchor(anchors, id, FlowEdgeKind::ExpressionShortMerge);
    require(evaluate.encoding == X64ExpressionBranchEncoding::ConditionalRel32 && !evaluate.conditional_taken,
        "C01 RHS edge is not the Jcc fallthrough path");
    require(short_edge.encoding == X64ExpressionBranchEncoding::ConditionalRel32 && short_edge.conditional_taken,
        "C01 short-circuit edge is not the Jcc taken path");
    require(evaluate.instruction_offset == short_edge.instruction_offset &&
            evaluate.displacement_offset == short_edge.displacement_offset,
        "C01 complementary paths do not share the same conditional instruction");
    require(rhs_merge.encoding == X64ExpressionBranchEncoding::UnconditionalRel32 &&
            short_merge.encoding == X64ExpressionBranchEncoding::Fallthrough,
        "C01 RHS and short-result merge encodings are incorrect");
}

void test_c02_or_branch_uses_jnz_and_exact_anchors() {
    const auto fixture = compile_fixture(
        "module N114C02; public function Main(bool flag, bool rhs) returns bool { return flag || rhs; }");
    validate_fixture(fixture);
    const auto& graph = first_graph(fixture);
    require(graph.expression_controls.size() == 1U && graph.expression_controls.front().operator_text == "||",
        "C02 logical OR control identity is missing");
    const auto anchors = anchors_for_function(fixture.codegen.flow_expression_branch_anchors);
    require(anchors.size() == 4U, "C02 logical OR must emit four edge anchors");
    const auto& taken = find_anchor(anchors, graph.expression_controls.front().id,
                                    FlowEdgeKind::ExpressionShortCircuit);
    require(fixture.code[taken.instruction_offset] == 0x0FU &&
            fixture.code[taken.instruction_offset + 1U] == 0x85U,
        "C02 logical OR does not use JNZ for its short-circuit path");
}

void test_c03_constant_false_and_keeps_machine_anchors_for_infeasible_rhs() {
    const auto fixture = compile_fixture(
        "module N114C03; public function Main(int value) returns bool { return false && (value > 0); }");
    validate_fixture(fixture);
    validate_occupancy_fixture(fixture);
    require(occupancy_for_function(fixture.codegen.flow_expression_edge_occupancy_captures).size() == 4U,
        "C03 analysis-infeasible RHS did not retain four machine-emission occupancy captures");
    const auto& graph = first_graph(fixture);
    require(graph.expression_controls.size() == 1U, "C03 expression control is missing");
    const auto id = graph.expression_controls.front().id;
    const auto rhs = std::find_if(graph.edges.begin(), graph.edges.end(), [id](const FlowEdge& edge) {
        return edge.domain == FlowEdgeDomain::Expression && edge.expression_id == id &&
            edge.kind == FlowEdgeKind::ExpressionEvaluateRhs;
    });
    require(rhs != graph.edges.end() && !rhs->analysis_feasible && rhs->machine_emission_required,
        "C03 RHS edge must be analysis-infeasible while remaining machine-emitted");
    require(anchors_for_function(fixture.codegen.flow_expression_branch_anchors).size() == 4U,
        "C03 machine anchors were incorrectly omitted for a constant-infeasible RHS");
}

void test_c04_constant_true_or_keeps_machine_anchors_for_infeasible_rhs() {
    const auto fixture = compile_fixture(
        "module N114C04; public function Main(int value) returns bool { return true || (value > 0); }");
    validate_fixture(fixture);
    validate_occupancy_fixture(fixture);
    require(occupancy_for_function(fixture.codegen.flow_expression_edge_occupancy_captures).size() == 4U,
        "C04 analysis-infeasible RHS did not retain four machine-emission occupancy captures");
    const auto& graph = first_graph(fixture);
    const auto id = graph.expression_controls.front().id;
    const auto rhs = std::find_if(graph.edges.begin(), graph.edges.end(), [id](const FlowEdge& edge) {
        return edge.domain == FlowEdgeDomain::Expression && edge.expression_id == id &&
            edge.kind == FlowEdgeKind::ExpressionEvaluateRhs;
    });
    require(rhs != graph.edges.end() && !rhs->analysis_feasible && rhs->machine_emission_required,
        "C04 OR RHS edge must remain in the machine-emission domain despite constant infeasibility");
    require(anchors_for_function(fixture.codegen.flow_expression_branch_anchors).size() == 4U,
        "C04 omitted machine anchors for a constant-infeasible RHS");
}

void test_c05_nested_logical_expressions_have_independent_anchor_groups() {
    const auto fixture = compile_fixture(
        "module N114C05; public function Main(bool a, bool b, bool c) returns bool { return a && (b || c); }");
    validate_fixture(fixture);
    validate_occupancy_fixture(fixture);
    require(occupancy_for_function(fixture.codegen.flow_expression_edge_occupancy_captures).size() == 8U,
        "C05 nested logical expressions did not produce eight independent occupancy captures");
    const auto& graph = first_graph(fixture);
    const auto anchors = anchors_for_function(fixture.codegen.flow_expression_branch_anchors);
    require(graph.expression_controls.size() == 2U && anchors.size() == 8U,
        "C05 nested logical expressions must have two independent four-anchor groups");
    require(graph.expression_controls[0].ast_expression != graph.expression_controls[1].ast_expression,
        "C05 nested FlowExpression controls alias the same AST node");
    require(graph.expression_controls[0].id != graph.expression_controls[1].id,
        "C05 nested logical expressions reuse a FlowExpressionId");
}

void test_c06_call_in_rhs_is_anchored_without_cross_binding() {
    const auto fixture = compile_fixture(
        "module N114C06; public function Main(bool flag) returns bool { return flag && IsReady(); } "
        "private function IsReady() returns bool { return true; }");
    validate_fixture(fixture);
    const auto main_anchors = anchors_for_function(fixture.codegen.flow_expression_branch_anchors, 0U);
    const auto helper_anchors = anchors_for_function(fixture.codegen.flow_expression_branch_anchors, 1U);
    require(main_anchors.size() == 4U && helper_anchors.empty(),
        "C06 expression anchors were attached to the wrong function");
    require(std::any_of(fixture.codegen.native_object.object_file.relocations.begin(),
        fixture.codegen.native_object.object_file.relocations.end(), [](const ObjectRelocation& relocation) {
            return relocation.kind == ObjectRelocationKind::CoffRelative32;
        }), "C06 RHS Call did not emit its expected direct-call relocation");
}

void test_c07_multiple_logical_expressions_use_exact_ast_identity_not_span_matching() {
    const auto fixture = compile_fixture(
        "module N114C07; public function Main(bool flag, bool a, bool b) returns bool { "
        "return (flag && a) || (!flag && b); }");
    validate_fixture(fixture);
    const auto& graph = first_graph(fixture);
    const auto anchors = anchors_for_function(fixture.codegen.flow_expression_branch_anchors);
    require(graph.expression_controls.size() == 3U && anchors.size() == 12U,
        "C07 expected three logical AST controls and twelve independent Edge anchors");
    std::set<const Expression*> ast_nodes;
    std::set<FlowExpressionId> ids;
    for (const auto& control : graph.expression_controls) {
        require(control.ast_expression != nullptr && ast_nodes.insert(control.ast_expression).second &&
                ids.insert(control.id).second,
            "C07 exact AST/FlowExpression identity is missing or duplicated");
    }
}

void test_c08_stack_probe_insertion_rebases_all_expression_offsets() {
    std::string source = "module N114C08; public function Main(bool flag) returns bool { ";
    for (std::size_t index = 0U; index < 530U; ++index) {
        source += "int local" + std::to_string(index) + " = " + std::to_string(index % 7U) + "; ";
    }
    source += "return flag && flag; }";
    const auto fixture = compile_fixture(source);
    validate_occupancy_fixture(fixture);
    const auto captures = occupancy_for_function(fixture.codegen.flow_expression_edge_occupancy_captures);
    require(captures.size() == 4U && std::all_of(captures.begin(), captures.end(),
        [](const X64ExpressionEdgeOccupancyCapture& capture) { return capture.code_offset > capture.original_code_offset; }),
        "C08 Stack Probe Prefix did not rebase all four occupancy captures");
    require(fixture.codegen.stats.win64_stack_probe_functions == 1U,
        "C08 did not trigger the Win64 stack-probe insertion path");
    validate_fixture(fixture);
    const auto anchors = anchors_for_function(fixture.codegen.flow_expression_branch_anchors);
    require(anchors.size() == 4U, "C08 stack-probe insertion lost expression branch anchors");
    for (const auto& anchor : anchors) {
        require(anchor.instruction_offset >= 15U && anchor.target_offset <= anchor.function_code_size &&
                anchor.function_code_size == fixture.code.size(),
            "C08 expression offsets were not rebased after the stack-probe prefix insertion");
    }
}

std::string n113_pressure_with_expression_source() {
    return "module N114C09; "
           "public function Main() returns int { "
           "int a=1; int b=2; int c=3; int d=4; int e=5; int f=6; "
           "double x=1.0; double y=2.0; double z=3.0; double w=4.0; "
           "double total = FloatHelper(x+y, z+w); "
           "int i=0; int result=0; "
           "while (i < 4) { if ((i < 2) && (a > 0)) { result = result + Helper(a,b); } "
           "else { result = result + Helper(c,d); } i = i + 1; } "
           "if (total > 0.0 && x > 0.0) { result = result + 1; } "
           "return result + e + f; } "
           "private function Helper(int p, int q) returns int { return p + q; } "
           "private function FloatHelper(double x, double y) returns double { return x + y; } "
           "private function Unused() returns int { return 77; }";
}

void test_c09_n113_action_insertion_rebases_expression_anchors_without_domain_leak() {
    const auto unit = parse_unit(n113_pressure_with_expression_source());
    X64CodegenOptions options{"n114_b13b_c09_g2", make_coff_x86_64_object_target()};
    options.insert_n113_emitter_spill_reload_actions = true;
    const auto result = compile_x64_coff_object(unit, options);
    require(result.succeeded(), result.diagnostics.empty() ? "C09 G2 compile failed" :
        "C09 G2 compile failed: " + result.diagnostics.front().code + " " + result.diagnostics.front().message);
    require(result.stats.native_n113_emitter_actions_inserted > 0U &&
            result.stats.native_n113_emitter_transfer_bytes_inserted > 0U,
        "C09 did not exercise real N113 machine-action code insertion");
    require(!result.flow_expression_branch_anchors.empty(),
        "C09 lost expression branch anchors after N113 insertion");
    const auto parsed_flow = analyze_control_flow(unit);
    require(parsed_flow.succeeded(), "C09 independent Flow graph build failed");
    for (const auto& graph : parsed_flow.functions) {
        const auto selected = anchors_for_function(result.flow_expression_branch_anchors,
            static_cast<std::size_t>(&graph - parsed_flow.functions.data()));
        const auto code_section = std::find_if(result.native_object.object_file.sections.begin(),
            result.native_object.object_file.sections.end(), [](const ObjectSection& section) {
                return section.kind == ObjectSectionKind::Text;
            });
        if (!selected.empty()) {
            require(code_section != result.native_object.object_file.sections.end(),
                "C09 emitted expression anchors without a function text section");
            const auto validation = validate_x64_expression_branch_anchors(
                code_section->contents, graph.expression_controls, graph.edges, selected,
                static_cast<std::size_t>(&graph - parsed_flow.functions.data()));
            require(validation.succeeded, validation.diagnostics.empty()
                ? "C09 post-insertion expression-anchor proof failed" : validation.diagnostics.front());
            const auto occupancy = occupancy_for_function(
                result.flow_expression_edge_occupancy_captures,
                static_cast<std::size_t>(&graph - parsed_flow.functions.data()));
            const auto occupancy_validation = validate_x64_expression_edge_occupancy_captures(
                code_section->contents, graph.expression_controls, graph.edges, selected, occupancy,
                static_cast<std::size_t>(&graph - parsed_flow.functions.data()));
            require(occupancy_validation.succeeded, occupancy_validation.diagnostics.empty()
                ? "C09 post-G2 Expression occupancy proof failed" : occupancy_validation.diagnostics.front());
        }
    }
    for (const auto& anchor : result.flow_branch_anchors) {
        require(anchor.edge_kind != FlowEdgeKind::ExpressionEvaluateRhs &&
                anchor.edge_kind != FlowEdgeKind::ExpressionShortCircuit &&
                anchor.edge_kind != FlowEdgeKind::ExpressionRhsMerge &&
                anchor.edge_kind != FlowEdgeKind::ExpressionShortMerge,
            "C09 Expression edge leaked into the N113 Statement Branch Anchor domain");
    }
}

void test_c10_missing_duplicate_and_unbound_identity_fail_closed() {
    const auto fixture = compile_fixture(
        "module N114C10; public function Main(bool flag, bool rhs) returns bool { return flag && rhs; }");
    const auto& graph = first_graph(fixture);
    auto anchors = anchors_for_function(fixture.codegen.flow_expression_branch_anchors);
    const auto valid = validate_x64_expression_branch_anchors(
        fixture.code, graph.expression_controls, graph.edges, anchors, 0U);
    require(valid.succeeded, "C10 baseline valid expression anchors were rejected");

    auto missing = anchors;
    missing.pop_back();
    require(!validate_x64_expression_branch_anchors(
        fixture.code, graph.expression_controls, graph.edges, missing, 0U).succeeded,
        "C10 validator accepted a missing Expression Anchor");

    auto duplicate = anchors;
    duplicate.push_back(duplicate.front());
    require(!validate_x64_expression_branch_anchors(
        fixture.code, graph.expression_controls, graph.edges, duplicate, 0U).succeeded,
        "C10 validator accepted a duplicate FlowEdgeId Anchor");

    auto unbound = graph.expression_controls;
    unbound.front().ast_expression = nullptr;
    require(!validate_x64_expression_branch_anchors(
        fixture.code, unbound, graph.edges, anchors, 0U).succeeded,
        "C10 validator accepted a missing AST-to-FlowExpression binding");
}

void test_c11_malformed_rel32_and_wrong_opcode_fail_closed() {
    const auto fixture = compile_fixture(
        "module N114C11; public function Main(bool flag, bool rhs) returns bool { return flag || rhs; }");
    const auto& graph = first_graph(fixture);
    auto anchors = anchors_for_function(fixture.codegen.flow_expression_branch_anchors);
    auto corrupted = anchors;
    auto taken = std::find_if(corrupted.begin(), corrupted.end(), [](const X64ExpressionBranchAnchor& anchor) {
        return anchor.edge_kind == FlowEdgeKind::ExpressionShortCircuit;
    });
    require(taken != corrupted.end(), "C11 test could not locate the Jcc taken anchor");
    ++taken->target_offset;
    require(!validate_x64_expression_branch_anchors(
        fixture.code, graph.expression_controls, graph.edges, corrupted, 0U).succeeded,
        "C11 validator accepted a mismatched conditional rel32 target");

    auto wrong_opcode = fixture.code;
    const auto rhs_edge = std::find_if(anchors.begin(), anchors.end(), [](const X64ExpressionBranchAnchor& anchor) {
        return anchor.edge_kind == FlowEdgeKind::ExpressionEvaluateRhs;
    });
    require(rhs_edge != anchors.end(), "C11 test could not locate the Jcc fallthrough anchor");
    wrong_opcode[rhs_edge->instruction_offset + 1U] = 0x84U;
    require(!validate_x64_expression_branch_anchors(
        wrong_opcode, graph.expression_controls, graph.edges, anchors, 0U).succeeded,
        "C11 validator accepted JZ opcode for a logical OR expression");
}

void test_c12_statement_anchors_remain_separate_from_expression_anchors() {
    const auto fixture = compile_fixture(
        "module N114C12; public function Main(bool flag, bool rhs) returns int { "
        "if (flag && rhs) { return 1; } else { return 0; } }");
    validate_fixture(fixture);
    require(!fixture.codegen.flow_branch_anchors.empty(),
        "C12 statement if branch anchors were lost");
    require(fixture.codegen.flow_expression_branch_anchors.size() == 4U,
        "C12 logical expression branch anchors are missing");
    for (const auto& anchor : fixture.codegen.flow_branch_anchors) {
        require(anchor.edge_kind != FlowEdgeKind::ExpressionEvaluateRhs &&
                anchor.edge_kind != FlowEdgeKind::ExpressionShortCircuit &&
                anchor.edge_kind != FlowEdgeKind::ExpressionRhsMerge &&
                anchor.edge_kind != FlowEdgeKind::ExpressionShortMerge,
            "C12 statement anchor vector contains an Expression-domain edge kind");
    }
}

void test_c13_occupancy_captures_all_four_edges_and_distinguishes_empty() {
    const auto fixture = compile_fixture(
        "module N114C13; public function Main(bool flag, bool rhs) returns bool { return flag && rhs; }");
    validate_occupancy_fixture(fixture);
    const auto& graph = first_graph(fixture);
    const auto captures = occupancy_for_function(fixture.codegen.flow_expression_edge_occupancy_captures);
    require(captures.size() == 4U, "C13 expected exactly four occupancy records");
    const FlowExpressionId id = graph.expression_controls.front().id;
    const auto& evaluate = find_occupancy(captures, id, FlowEdgeKind::ExpressionEvaluateRhs);
    const auto& short_edge = find_occupancy(captures, id, FlowEdgeKind::ExpressionShortCircuit);
    const auto& rhs_merge = find_occupancy(captures, id, FlowEdgeKind::ExpressionRhsMerge);
    const auto& short_merge = find_occupancy(captures, id, FlowEdgeKind::ExpressionShortMerge);
    require(evaluate.edge_id != short_edge.edge_id && evaluate.capture_complete && short_edge.capture_complete &&
            rhs_merge.capture_complete && short_merge.capture_complete,
        "C13 empty occupancy was not represented by four complete independent records");
    require(evaluate.capture_site == X64ExpressionEdgeCaptureSite::AfterConditionalJccBeforeRhs &&
            short_edge.capture_site == X64ExpressionEdgeCaptureSite::AfterConditionalJccBeforeRhs &&
            rhs_merge.capture_site == X64ExpressionEdgeCaptureSite::AfterRhsNormalizeBeforeMergeJump &&
            short_merge.capture_site == X64ExpressionEdgeCaptureSite::AfterShortResultBeforeFallthrough,
        "C13 capture-site contract does not match the three production seams");
    for (const auto& capture : captures) {
        require(capture.gp_depth == 0U && capture.xmm_depth == 0U &&
                capture.gp_stack_order.empty() && capture.xmm_stack_order.empty() &&
                capture.gp_occupancy.empty() && capture.xmm_occupancy.empty(),
            "C13 valid captured-empty occupancy was not preserved as an explicit zero-depth record");
        require(capture.original_code_offset == capture.code_offset,
            "C13 no-insertion capture unexpectedly changed its code boundary");
    }
    require(evaluate.original_code_offset == short_edge.original_code_offset &&
            evaluate.gp_stack_order == short_edge.gp_stack_order &&
            evaluate.xmm_stack_order == short_edge.xmm_stack_order &&
            rhs_merge.gp_stack_order == short_merge.gp_stack_order &&
            rhs_merge.xmm_stack_order == short_merge.xmm_stack_order,
        "C13 JCC sibling or merge predecessor payloads differ");
}

void test_c14_prefix_then_real_g2_rebases_nonempty_gp_occupancy() {
    std::string source = "module N114C14; "
        "public function Main(bool flag, bool rhs_bool) returns int { ";
    for (std::size_t index = 0U; index < 530U; ++index)
        source += "int large_frame_slot" + std::to_string(index) + " = " + std::to_string(index % 7U) + "; ";
    source += "int a=1; int b=2; int c=3; int d=4; int e=5; int f=6; "
              "double x=1.0; double y=2.0; double z=3.0; double w=4.0; "
              "double total = FloatHelper(x+y, z+w); int result=0; "
              "if ((a == b) == (flag && rhs_bool)) { result = result + Helper(a,b); } "
              "int i=0; while (i < 4) { if (i < 2) { result = result + Helper(a,b); } "
              "else { result = result + Helper(c,d); } i=i+1; } "
              "if (total > 0.0) { result = result + 1; } return result + e + f; } "
              "private function Helper(int p, int q) returns int { return p + q; } "
              "private function FloatHelper(double p, double q) returns double { return p + q; }";
    X64CodegenOptions options{"n114_b13b_c14_prefix_g2_occupancy", make_coff_x86_64_object_target()};
    options.insert_n113_emitter_spill_reload_actions = true;
    const auto fixture = compile_fixture(source, options);
    require(fixture.codegen.stats.win64_stack_probe_functions == 1U,
        "C14 did not trigger the real Win64 Stack Probe Prefix");
    require(fixture.codegen.stats.native_n113_emitter_actions_inserted > 0U &&
            fixture.codegen.stats.native_n113_emitter_transfer_bytes_inserted > 0U,
        "C14 did not trigger real G2 machine transfer insertion");
    const auto& graph = first_graph(fixture);
    const auto captures = occupancy_for_function(fixture.codegen.flow_expression_edge_occupancy_captures, 0U);
    require(graph.expression_controls.size() == 1U && captures.size() == 4U,
        "C14 Main does not have one complete four-Edge occupancy group");
    validate_occupancy_fixture(fixture);
    bool has_nonempty_gp = false;
    for (const auto& capture : captures) {
        require(capture.capture_complete && capture.code_offset <= fixture.code.size() &&
                capture.code_offset >= capture.original_code_offset,
            "C14 capture was not completely/compositionally rebased into final function bounds");
        require(capture.xmm_depth == 0U && capture.xmm_stack_order.empty(),
            "C14 changed the known captured-empty XMM state");
        if (capture.gp_depth > 0U) has_nonempty_gp = true;
    }
    require(has_nonempty_gp, "C14 failed to observe a live GP temporary across the logical expression");
}

void test_c15_occupancy_validator_fails_closed_on_missing_duplicate_and_payload_corruption() {
    const auto fixture = compile_fixture(
        "module N114C15; public function Main(bool flag, bool rhs) returns bool { return flag || rhs; }");
    const auto& graph = first_graph(fixture);
    const auto anchors = anchors_for_function(fixture.codegen.flow_expression_branch_anchors);
    auto captures = occupancy_for_function(fixture.codegen.flow_expression_edge_occupancy_captures);
    require(validate_x64_expression_edge_occupancy_captures(
        fixture.code, graph.expression_controls, graph.edges, anchors, captures, 0U).succeeded,
        "C15 baseline occupancy records were rejected");
    auto missing = captures;
    missing.pop_back();
    require(!validate_x64_expression_edge_occupancy_captures(
        fixture.code, graph.expression_controls, graph.edges, anchors, missing, 0U).succeeded,
        "C15 validator accepted a missing occupancy record");
    auto duplicate = captures;
    duplicate.push_back(duplicate.front());
    require(!validate_x64_expression_edge_occupancy_captures(
        fixture.code, graph.expression_controls, graph.edges, anchors, duplicate, 0U).succeeded,
        "C15 validator accepted a duplicate EdgeId occupancy record");
    auto depth_mismatch = captures;
    depth_mismatch.front().gp_depth = 1U;
    require(!validate_x64_expression_edge_occupancy_captures(
        fixture.code, graph.expression_controls, graph.edges, anchors, depth_mismatch, 0U).succeeded,
        "C15 validator accepted a depth/stack mismatch");
    auto incomplete = captures;
    incomplete.front().capture_complete = false;
    require(!validate_x64_expression_edge_occupancy_captures(
        fixture.code, graph.expression_controls, graph.edges, anchors, incomplete, 0U).succeeded,
        "C15 validator accepted an incomplete capture");
    auto wrong_identity = captures;
    ++wrong_identity.front().from_block_id;
    require(!validate_x64_expression_edge_occupancy_captures(
        fixture.code, graph.expression_controls, graph.edges, anchors, wrong_identity, 0U).succeeded,
        "C15 validator accepted a mismatched FlowEdge endpoint");
    auto wrong_site = captures;
    wrong_site.front().capture_site = X64ExpressionEdgeCaptureSite::AfterShortResultBeforeFallthrough;
    require(!validate_x64_expression_edge_occupancy_captures(
        fixture.code, graph.expression_controls, graph.edges, anchors, wrong_site, 0U).succeeded,
        "C15 validator accepted a capture at the wrong production seam");
    auto wrong_register = captures;
    wrong_register.front().gp_stack_order = {16U};
    wrong_register.front().gp_occupancy = {16U};
    wrong_register.front().gp_depth = 1U;
    require(!validate_x64_expression_edge_occupancy_captures(
        fixture.code, graph.expression_controls, graph.edges, anchors, wrong_register, 0U).succeeded,
        "C15 validator accepted an out-of-range physical register");
    auto wrong_merge_stack = captures;
    auto rhs_merge = std::find_if(wrong_merge_stack.begin(), wrong_merge_stack.end(), [](
        const X64ExpressionEdgeOccupancyCapture& capture) {
        return capture.edge_kind == FlowEdgeKind::ExpressionRhsMerge;
    });
    require(rhs_merge != wrong_merge_stack.end(), "C15 could not find RHS merge capture");
    rhs_merge->gp_stack_order = {8U}; rhs_merge->gp_occupancy = {8U}; rhs_merge->gp_depth = 1U;
    require(!validate_x64_expression_edge_occupancy_captures(
        fixture.code, graph.expression_controls, graph.edges, anchors, wrong_merge_stack, 0U).succeeded,
        "C15 validator accepted different ordered occupancy at the two merge predecessors");
}

void test_c16_occupancy_before_rebase_affinity_and_overflow_are_checked() {
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> adjustments{
        {90U, 5U}, {100U, 7U}, {110U, 9U}};
    const auto equal_boundary = rebase_x64_expression_edge_occupancy_offset_before(
        100U, 200U, adjustments, 221U);
    require(equal_boundary.succeeded && equal_boundary.mapped_offset == 105U,
        "C16 BEFORE affinity must shift earlier insertion only, not equal or later insertion");
    const auto after_all = rebase_x64_expression_edge_occupancy_offset_before(
        120U, 200U, adjustments, 221U);
    require(after_all.succeeded && after_all.mapped_offset == 141U,
        "C16 capture after every adjustment did not compose all inserted byte groups");
    const auto before_all = rebase_x64_expression_edge_occupancy_offset_before(
        50U, 200U, adjustments, 221U);
    require(before_all.succeeded && before_all.mapped_offset == 50U,
        "C16 capture before every adjustment unexpectedly moved");
    const auto overflow = rebase_x64_expression_edge_occupancy_offset_before(
        std::numeric_limits<std::uint32_t>::max() - 4U,
        std::numeric_limits<std::uint32_t>::max(), {{1U, 8U}},
        std::numeric_limits<std::uint32_t>::max());
    require(!overflow.succeeded, "C16 checked BEFORE mapper accepted uint32 overflow");
    const auto unsorted = rebase_x64_expression_edge_occupancy_offset_before(
        100U, 200U, {{90U, 5U}, {80U, 7U}}, 212U);
    require(!unsorted.succeeded, "C16 checked BEFORE mapper accepted unsorted insertion adjustments");
    const auto wrong_size = rebase_x64_expression_edge_occupancy_offset_before(
        100U, 200U, adjustments, 220U);
    require(!wrong_size.succeeded, "C16 checked BEFORE mapper accepted inconsistent final function size");
}

void test_c17_nested_expression_occupancy_groups_are_independent_and_deterministic() {
    const auto fixture = compile_fixture(
        "module N114C17; public function Main(bool a, bool b, bool c) returns bool { return a && (b || c); }");
    validate_occupancy_fixture(fixture);
    const auto& graph = first_graph(fixture);
    const auto captures = occupancy_for_function(fixture.codegen.flow_expression_edge_occupancy_captures);
    require(graph.expression_controls.size() == 2U && captures.size() == 8U,
        "C17 nested logical expressions did not produce independent four-record groups");
    for (const auto& control : graph.expression_controls) {
        std::set<FlowEdgeId> ids;
        for (const auto& capture : captures)
            if (capture.expression_id == control.id) ids.insert(capture.edge_id);
        require(ids.size() == 4U, "C17 one nested expression reused/missed an EdgeId");
    }
    require(std::is_sorted(fixture.codegen.flow_expression_edge_occupancy_captures.begin(),
        fixture.codegen.flow_expression_edge_occupancy_captures.end(),
        [](const X64ExpressionEdgeOccupancyCapture& left, const X64ExpressionEdgeOccupancyCapture& right) {
            if (left.function_index != right.function_index) return left.function_index < right.function_index;
            if (left.expression_id != right.expression_id) return left.expression_id < right.expression_id;
            return left.edge_id < right.edge_id;
        }), "C17 occupancy output is not deterministically sorted by function/expression/edge");
}

void test_c18_expression_occupancy_remains_separate_from_statement_anchor_domain() {
    const auto fixture = compile_fixture(
        "module N114C18; public function Main(bool flag, bool rhs) returns int { "
        "if (flag && rhs) { return 1; } else { return 0; } }");
    validate_occupancy_fixture(fixture);
    require(fixture.codegen.flow_expression_edge_occupancy_captures.size() == 4U,
        "C18 expression occupancy records are missing");
    for (const auto& anchor : fixture.codegen.flow_branch_anchors) {
        require(anchor.edge_kind != FlowEdgeKind::ExpressionEvaluateRhs &&
                anchor.edge_kind != FlowEdgeKind::ExpressionShortCircuit &&
                anchor.edge_kind != FlowEdgeKind::ExpressionRhsMerge &&
                anchor.edge_kind != FlowEdgeKind::ExpressionShortMerge,
            "C18 Expression occupancy leaked into the Statement-edge branch-anchor domain");
    }
}

void test_c19_occupancy_records_keep_nonzero_function_identity() {
    const auto fixture = compile_fixture(
        "module N114C19; public function Main() returns bool { return Helper(true, false); } "
        "private function Helper(bool a, bool b) returns bool { return a && b; }");
    const auto captures = fixture.codegen.flow_expression_edge_occupancy_captures;
    require(captures.size() == 4U, "C19 expected one helper-function occupancy group");
    for (const auto& capture : captures)
        require(capture.function_index == 1U && capture.expression_id != InvalidFlowExpressionId,
            "C19 occupancy capture was assigned to the wrong function index");
    require(std::is_sorted(captures.begin(), captures.end(),
        [](const X64ExpressionEdgeOccupancyCapture& left, const X64ExpressionEdgeOccupancyCapture& right) {
            if (left.function_index != right.function_index) return left.function_index < right.function_index;
            if (left.expression_id != right.expression_id) return left.expression_id < right.expression_id;
            return left.edge_id < right.edge_id;
        }), "C19 nonzero-function captures are not deterministically sorted");
}

void test_c20_functions_without_logical_expressions_emit_no_occupancy_records() {
    const auto fixture = compile_fixture(
        "module N114C20; public function Main(int value) returns int { return value + 1; }");
    require(fixture.codegen.flow_expression_edge_occupancy_captures.empty(),
        "C20 function without logical expressions emitted an orphan occupancy record");
}



void require_xmm_live_across_nested_call_argument(const std::string& module_name,
                                                  const std::string& logical_operator,
                                                  const std::string& case_name) {
    const std::string source =
        "module " + module_name + "; "
        "public function Main(float left, float right, bool flag, bool rhs) returns float { "
        "return left + Blend(right, flag " + logical_operator + " rhs); } "
        "private function Blend(float value, bool condition) returns float { return value; }";
    const auto fixture = compile_fixture(source);
    validate_fixture(fixture);
    validate_occupancy_fixture(fixture);

    const auto& graph = first_graph(fixture);
    const auto captures = occupancy_for_function(
        fixture.codegen.flow_expression_edge_occupancy_captures, 0U);
    require(graph.expression_controls.size() == 1U && captures.size() == 4U,
        case_name + " did not produce exactly one four-edge expression occupancy group");

    const auto& expected_xmm_stack = captures.front().xmm_stack_order;
    require(!expected_xmm_stack.empty(),
        case_name + " did not keep the outer floating-point temporary live at the nested logical seams");
    for (const auto reg : expected_xmm_stack) {
        require(reg >= 6U && reg <= 15U,
            case_name + " did not preserve the call-crossing floating temporary in a Win64 nonvolatile XMM register");
    }
    for (const auto& capture : captures) {
        require(capture.capture_complete && capture.xmm_depth > 0U &&
                capture.xmm_stack_order == expected_xmm_stack &&
                capture.xmm_depth == expected_xmm_stack.size() &&
                capture.xmm_occupancy.size() == expected_xmm_stack.size(),
            case_name + " did not retain the same live XMM occupancy at all four Expression edges");
    }
}

void test_c21_xmm_live_across_nested_and_call_argument() {
    require_xmm_live_across_nested_call_argument(
        "N114C21", "&&", "C21 nested AND call-argument fixture");
}

void test_c22_xmm_live_across_nested_or_call_argument() {
    require_xmm_live_across_nested_call_argument(
        "N114C22", "||", "C22 nested OR call-argument fixture");
}

} // namespace

void run_n114_b13b_expression_branch_anchor_tests(int& passed, int& failed) {
    run_case("N114B13BC01_and_four_anchors", test_c01_and_branch_has_exact_four_machine_anchors, passed, failed);
    run_case("N114B13BC02_or_jnz_anchors", test_c02_or_branch_uses_jnz_and_exact_anchors, passed, failed);
    run_case("N114B13BC03_constant_false_rhs_anchor", test_c03_constant_false_and_keeps_machine_anchors_for_infeasible_rhs, passed, failed);
    run_case("N114B13BC04_constant_true_rhs_anchor", test_c04_constant_true_or_keeps_machine_anchors_for_infeasible_rhs, passed, failed);
    run_case("N114B13BC05_nested_expression_groups", test_c05_nested_logical_expressions_have_independent_anchor_groups, passed, failed);
    run_case("N114B13BC06_call_rhs_anchor", test_c06_call_in_rhs_is_anchored_without_cross_binding, passed, failed);
    run_case("N114B13BC07_exact_ast_identity", test_c07_multiple_logical_expressions_use_exact_ast_identity_not_span_matching, passed, failed);
    run_case("N114B13BC08_stack_probe_rebase", test_c08_stack_probe_insertion_rebases_all_expression_offsets, passed, failed);
    run_case("N114B13BC09_n113_insertion_domain", test_c09_n113_action_insertion_rebases_expression_anchors_without_domain_leak, passed, failed);
    run_case("N114B13BC10_missing_duplicate_identity", test_c10_missing_duplicate_and_unbound_identity_fail_closed, passed, failed);
    run_case("N114B13BC11_malformed_rel32_opcode", test_c11_malformed_rel32_and_wrong_opcode_fail_closed, passed, failed);
    run_case("N114B13BC12_statement_anchor_isolation", test_c12_statement_anchors_remain_separate_from_expression_anchors, passed, failed);
    run_case("N114B13BC13_four_edge_occupancy_captured_empty", test_c13_occupancy_captures_all_four_edges_and_distinguishes_empty, passed, failed);
    run_case("N114B13BC14_prefix_g2_nonempty_gp_occupancy", test_c14_prefix_then_real_g2_rebases_nonempty_gp_occupancy, passed, failed);
    run_case("N114B13BC15_occupancy_validator_fail_closed", test_c15_occupancy_validator_fails_closed_on_missing_duplicate_and_payload_corruption, passed, failed);
    run_case("N114B13BC16_before_rebase_affinity_checked", test_c16_occupancy_before_rebase_affinity_and_overflow_are_checked, passed, failed);
    run_case("N114B13BC17_nested_occupancy_deterministic", test_c17_nested_expression_occupancy_groups_are_independent_and_deterministic, passed, failed);
    run_case("N114B13BC18_expression_statement_domain_isolation", test_c18_expression_occupancy_remains_separate_from_statement_anchor_domain, passed, failed);
    run_case("N114B13BC19_nonzero_function_identity", test_c19_occupancy_records_keep_nonzero_function_identity, passed, failed);
    run_case("N114B13BC20_no_expression_records", test_c20_functions_without_logical_expressions_emit_no_occupancy_records, passed, failed);
    run_case("N114B13BC21_xmm_live_across_nested_and_call_argument", test_c21_xmm_live_across_nested_and_call_argument, passed, failed);
    run_case("N114B13BC22_xmm_live_across_nested_or_call_argument", test_c22_xmm_live_across_nested_or_call_argument, passed, failed);
}
