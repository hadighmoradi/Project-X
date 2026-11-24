// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/control_flow.hpp"
#include "xfrontend/source_manager.hpp"
#include "xfrontend/semantic.hpp"
#include "test_report.hpp"

#include <chrono>

#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

void flow_require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

xfrontend::ControlFlowResult flow_result(const std::string& source) {
    const auto parsed = xfrontend::parse_source(source);
    if (!parsed.succeeded()) {
        throw std::runtime_error("test source failed to parse: " + parsed.diagnostics.front().code +
                                 " " + parsed.diagnostics.front().message);
    }
    return xfrontend::analyze_control_flow(parsed.unit);
}

bool has_flow_code(const xfrontend::ControlFlowResult& result, const std::string& code) {
    for (const auto& item : result.diagnostics) {
        if (item.code == code) return true;
    }
    return false;
}

void flow_case(const char* name, const std::function<void()>& body,
               int& passed, int& failed) {
    if (!xfrontend_test_report::should_run(name)) return;
    const auto start = std::chrono::steady_clock::now();
    try {
        body();
        ++passed;
        std::cout << "PASS " << name << '\n';
        xfrontend_test_report::record(name, true, "", std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
    } catch (const std::exception& error) {
        ++failed;
        std::cout << "FAIL " << name << ": " << error.what() << '\n';
        xfrontend_test_report::record(name, false, error.what(), std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
    }
}

void valid(const std::string& source) {
    const auto result = flow_result(source);
    flow_require(result.succeeded(), "unexpected flow diagnostic: " +
                 (result.diagnostics.empty() ? std::string() : result.diagnostics.front().message));
}

void invalid(const std::string& source) {
    const auto result = flow_result(source);
    flow_require(has_flow_code(result, "XFLOW0001"), "expected XFLOW0001 uninitialized-read diagnostic");
}

} // namespace

void run_control_flow_tests(int& passed, int& failed) {
    using namespace xfrontend;

    flow_case("F001_empty_unit", [&] {
        const auto result = flow_result("");
        flow_require(result.succeeded() && result.functions.empty(), "empty source should have no flow graphs");
    }, passed, failed);
    flow_case("F002_parameter_is_initialized", [&] {
        valid("function Read(int value) returns int { return value; }");
    }, passed, failed);
    flow_case("F003_uninitialized_local_read", [&] {
        invalid("function Read() returns int { int value; return value; }");
    }, passed, failed);
    flow_case("F004_initialized_local_read", [&] {
        valid("function Read() returns int { int value = 12; return value; }");
    }, passed, failed);
    flow_case("F005_assignment_initializes_local", [&] {
        valid("function Read() returns int { int value; value = 7; return value; }");
    }, passed, failed);
    flow_case("F006_read_before_assignment_rhs", [&] {
        invalid("function Read() returns int { int value; value = value + 1; return value; }");
    }, passed, failed);
    flow_case("F007_if_else_both_assign", [&] {
        valid("function Read(bool flag) returns int { int value; if (flag) { value = 1; } else { value = 2; } return value; }");
    }, passed, failed);
    flow_case("F008_if_without_else_not_definite", [&] {
        invalid("function Read(bool flag) returns int { int value; if (flag) { value = 1; } return value; }");
    }, passed, failed);
    flow_case("F009_if_else_one_branch_missing", [&] {
        invalid("function Read(bool flag) returns int { int value; if (flag) { value = 1; } else { PrintLine \"skip\"; } return value; }");
    }, passed, failed);
    flow_case("F010_each_branch_initializes", [&] {
        valid("function Read(bool flag) returns int { int value; if (flag) { value = 3; } else { value = 4; } PrintLine value; return value; }");
    }, passed, failed);
    flow_case("F011_nested_if_both_paths_assign", [&] {
        valid("function Read(bool a, bool b) returns int { int value; if (a) { if (b) { value = 1; } else { value = 2; } } else { value = 3; } return value; }");
    }, passed, failed);
    flow_case("F012_nested_if_missing_path", [&] {
        invalid("function Read(bool a, bool b) returns int { int value; if (a) { if (b) { value = 1; } } else { value = 3; } return value; }");
    }, passed, failed);
    flow_case("F013_while_assignment_not_definite_after_loop", [&] {
        invalid("function Read(bool flag) returns int { int value; while (flag) { value = 8; } return value; }");
    }, passed, failed);
    flow_case("F014_while_body_read_before_write", [&] {
        invalid("function Read(bool flag) { int value; while (flag) { PrintLine value; value = 1; } }");
    }, passed, failed);
    flow_case("F015_while_condition_reads_initialized_parameter", [&] {
        valid("function Read(bool flag) { while (flag) { break; } }");
    }, passed, failed);
    flow_case("F016_for_assignment_not_definite_after_loop", [&] {
        invalid("function Read() returns int { int value; for i in 0..3 { value = i; } return value; }");
    }, passed, failed);
    flow_case("F017_for_iterator_initialized_inside_body", [&] {
        valid("function Read() { for i in 0..3 { PrintLine i; } }");
    }, passed, failed);
    flow_case("F018_for_iterator_not_visible_after_loop", [&] {
        const auto parsed = parse_source("function Read() { for i in 0..3 { PrintLine i; } }");
        flow_require(parsed.succeeded(), "source should parse");
        const auto result = analyze_control_flow(parsed.unit);
        flow_require(result.succeeded(), "loop-local iterator should not create a flow read outside its scope");
    }, passed, failed);
    flow_case("F019_break_does_not_prove_loop_runs", [&] {
        invalid("function Read(bool flag) returns int { int value; while (flag) { value = 5; break; } return value; }");
    }, passed, failed);
    flow_case("F020_continue_path_keeps_zero_iteration", [&] {
        invalid("function Read(bool flag) returns int { int value; while (flag) { value = 5; continue; } return value; }");
    }, passed, failed);
    flow_case("F021_returning_else_branch_removes_unassigned_path", [&] {
        valid("function Read(bool flag) returns int { int value; if (flag) { value = 6; } else { return 0; } return value; }");
    }, passed, failed);
    flow_case("F022_return_terminates_current_path", [&] {
        valid("function Read(bool flag) returns int { int value; if (flag) { return 0; } value = 9; return value; }");
    }, passed, failed);
    flow_case("F023_unreachable_read_is_not_an_executable_path", [&] {
        const auto parsed = parse_source("function Read() returns int { return 1; int value; PrintLine value; }");
        flow_require(parsed.succeeded(), "source should parse");
        const auto result = analyze_control_flow(parsed.unit);
        flow_require(result.succeeded(), "unreachable statements must not affect reachable-path analysis");
    }, passed, failed);
    flow_case("F024_implicit_assignment_initializes", [&] {
        valid("function Read() returns int { value = 11; return value; }");
    }, passed, failed);
    flow_case("F025_implicit_child_local_does_not_escape", [&] {
        const auto parsed = parse_source("function Read(bool flag) { if (flag) { localValue = 1; PrintLine localValue; } }");
        flow_require(parsed.succeeded(), "source should parse");
        flow_require(analyze_control_flow(parsed.unit).succeeded(), "child local is initialized inside its lexical scope");
    }, passed, failed);
    flow_case("F026_outer_variable_written_in_nested_block", [&] {
        valid("function Read(bool flag) returns int { int value; if (flag) { value = 1; } else { value = 2; } return value; }");
    }, passed, failed);
    flow_case("F027_nested_block_uninitialized_read", [&] {
        invalid("function Read() { int value; { PrintLine value; } }");
    }, passed, failed);
    flow_case("F028_sibling_implicit_locals_are_independent", [&] {
        const auto parsed = parse_source("function Read(bool flag) { if (flag) { first = 1; } else { second = 2; } }");
        flow_require(parsed.succeeded(), "source should parse");
        flow_require(analyze_control_flow(parsed.unit).succeeded(), "sibling-scope locals should not leak");
    }, passed, failed);
    flow_case("F029_assignment_member_reads_receiver", [&] {
        invalid("function Read() { Object item; item.value = 1; }");
    }, passed, failed);
    flow_case("F030_assignment_index_reads_array_and_index", [&] {
        invalid("function Read() { List[int] items; int index = 0; items[index] = 1; }");
    }, passed, failed);
    flow_case("F031_call_argument_reads_variable", [&] {
        invalid("function Read() { int value; PrintLine value; }");
    }, passed, failed);
    flow_case("F032_method_receiver_is_read", [&] {
        invalid("function Read() { Object item; item.Refresh(); }");
    }, passed, failed);
    flow_case("F033_imported_namespace_is_not_local_variable", [&] {
        const auto parsed = parse_source("module App; import Api; function Main() { Api.Run(); }");
        flow_require(parsed.succeeded(), "source should parse");
        flow_require(analyze_control_flow(parsed.unit).succeeded(), "import root must not be treated as a local variable");
    }, passed, failed);
    flow_case("F034_function_callee_is_not_a_variable_read", [&] {
        const auto parsed = parse_source("function Helper() {} function Main() { Helper(); }");
        flow_require(parsed.succeeded(), "source should parse");
        flow_require(analyze_control_flow(parsed.unit).succeeded(), "function symbol should not be treated as a local variable");
    }, passed, failed);
    flow_case("F035_local_callable_variable_is_read", [&] {
        invalid("function Main() { int functionPlaceholder; functionPlaceholder(); }");
    }, passed, failed);
    flow_case("F036_separate_functions_have_independent_flow_state", [&] {
        valid("function First() { int value = 1; PrintLine value; } function Second() { int value; value = 2; PrintLine value; }");
    }, passed, failed);
    flow_case("F037_parameter_used_after_nested_scope", [&] {
        valid("function Read(int value) returns int { { PrintLine value; } return value; }");
    }, passed, failed);
    flow_case("F038_initialized_in_both_nested_branches", [&] {
        valid("function Read(bool a, bool b) returns int { int value; if (a) { if (b) { value = 1; } else { value = 2; } } else { if (b) { value = 3; } else { value = 4; } } return value; }");
    }, passed, failed);
    flow_case("F039_variable_uninitialized_on_one_nested_path", [&] {
        invalid("function Read(bool a, bool b) returns int { int value; if (a) { if (b) { value = 1; } else { value = 2; } } else { PrintLine b; } return value; }");
    }, passed, failed);
    flow_case("F040_loop_graph_contains_back_edge", [&] {
        const auto parsed = parse_source("function Loop(bool flag) { while (flag) { if (flag) { continue; } break; } }");
        flow_require(parsed.succeeded(), "source should parse");
        const auto result = analyze_control_flow(parsed.unit);
        flow_require(result.functions.size() == 1U, "one function graph expected");
        bool has_back_edge = false;
        for (const auto& block : result.functions[0].blocks) {
            for (const auto target : block.successors) {
                if (target <= block.id) has_back_edge = true;
            }
        }
        flow_require(has_back_edge, "CFG should contain a loop back edge");
    }, passed, failed);
    flow_case("F041_graph_has_entry_and_exit", [&] {
        const auto parsed = parse_source("function Main() { PrintLine \"ok\"; }");
        const auto result = analyze_control_flow(parsed.unit);
        flow_require(result.functions.size() == 1U, "one graph expected");
        const auto& graph = result.functions[0];
        flow_require(graph.entry_block != graph.exit_block, "entry and exit must be distinct");
        flow_require(graph.blocks[graph.entry_block].id == graph.entry_block, "entry id mismatch");
    }, passed, failed);
    flow_case("F042_source_manager_analysis", [&] {
        SourceManager sources;
        const SourceFileId id = sources.add_buffer("flow.src", "function Main() { int value; PrintLine value; }");
        const auto result = analyze_control_flow(sources, id);
        flow_require(has_flow_code(result, "XFLOW0001"), "source manager entry should run definite initialization");
        flow_require(result.diagnostics[0].span.source_id == id, "source id must be preserved");
    }, passed, failed);
    flow_case("F043_source_manager_parse_error_propagates", [&] {
        SourceManager sources;
        const SourceFileId id = sources.add_buffer("broken.src", "function Main( {");
        const auto result = analyze_control_flow(sources, id);
        flow_require(!result.succeeded(), "parse failure should propagate");
        flow_require(!result.diagnostics.empty() && result.diagnostics[0].code.find("XPARSE") == 0U,
                     "parser diagnostic expected");
        flow_require(result.diagnostics[0].span.source_id == id, "parse error source id should be preserved");
    }, passed, failed);
    flow_case("F044_persian_comments_and_strings_do_not_create_reads", [&] {
        valid(u8"function Main() { // متغیر ساختگی value\n PrintLine \"مقدار value\"; }");
    }, passed, failed);
    flow_case("F045_initializer_reads_parameter", [&] {
        valid("function Read(int input) returns int { int output = input; return output; }");
    }, passed, failed);
    flow_case("F046_assignment_in_all_branches_then_nested_read", [&] {
        valid("function Read(bool flag) { int value; if (flag) { value = 1; } else { value = 2; } { PrintLine value; } }");
    }, passed, failed);
    flow_case("F047_if_assignment_not_visible_in_else_branch", [&] {
        invalid("function Read(bool flag) { int value; if (flag) { value = 1; } else { PrintLine value; } }");
    }, passed, failed);
    flow_case("F048_loop_body_assignment_does_not_initialize_before_first_iteration", [&] {
        invalid("function Read(bool flag) { int value; while (flag) { PrintLine value; value = 1; } }");
    }, passed, failed);
    flow_case("F049_declared_but_uninitialized_is_killed", [&] {
        invalid("function Read(bool flag) { int value = 4; if (flag) { int value; PrintLine value; } }");
    }, passed, failed);
    flow_case("F050_cfg_preserves_source_identity", [&] {
        SourceManager sources;
        const SourceFileId id = sources.add_buffer("identity.src", "function Main() { int value; PrintLine value; }");
        const auto result = analyze_control_flow(sources, id);
        flow_require(!result.diagnostics.empty(), "diagnostic expected");
        flow_require(result.diagnostics.front().span.source_id == id, "diagnostic must identify source file");
    }, passed, failed);
    flow_case("F051_semantic_entry_point_runs_definite_initialization", [&] {
        const auto parsed = parse_source("function Main() { int value; PrintLine value; }");
        flow_require(parsed.succeeded(), "source should parse");
        const auto result = analyze_semantics(parsed.unit);
        bool found = false;
        for (const auto& diagnostic : result.diagnostics) {
            if (diagnostic.code == "XFLOW0001") found = true;
        }
        flow_require(found, "SemanticAnalyzer::run should include definite initialization");
    }, passed, failed);
    flow_case("F052_program_entry_point_runs_definite_initialization", [&] {
        const auto result = analyze_program({
            ProgramFile{"main.src", 77U, std::make_shared<CompilationUnit>(
                parse_source("module Main; function Start() { int value; PrintLine value; }").unit)}});
        bool found = false;
        for (const auto& diagnostic : result.diagnostics) {
            if (diagnostic.code == "XFLOW0001") found = true;
        }
        flow_require(found, "program semantic analysis should include definite initialization");
    }, passed, failed);

}
