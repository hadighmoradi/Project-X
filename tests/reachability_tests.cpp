// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/control_flow.hpp"
#include "xfrontend/semantic.hpp"
#include "xfrontend/source_manager.hpp"
#include "test_report.hpp"

#include <chrono>

#include <algorithm>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

void reach_require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

xfrontend::ControlFlowResult reach_result(const std::string& source) {
    const auto parsed = xfrontend::parse_source(source);
    if (!parsed.succeeded()) {
        throw std::runtime_error("test source failed to parse: " + parsed.diagnostics.front().code +
                                 " " + parsed.diagnostics.front().message);
    }
    return xfrontend::analyze_control_flow(parsed.unit);
}

bool has_code(const xfrontend::ControlFlowResult& result, const std::string& code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const xfrontend::Diagnostic& item) { return item.code == code; });
}

bool contains_unreachable(const xfrontend::ControlFlowGraph& graph) {
    return !graph.unreachable_blocks.empty();
}

void reach_case(const char* name, const std::function<void()>& body,
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
    const auto result = reach_result(source);
    reach_require(result.succeeded(), "unexpected flow diagnostic: " +
        (result.diagnostics.empty() ? std::string() : result.diagnostics.front().code + " " +
         result.diagnostics.front().message));
}

void missing_return(const std::string& source) {
    const auto result = reach_result(source);
    reach_require(has_code(result, "XFLOW0002"), "expected XFLOW0002 missing-return diagnostic");
}

} // namespace

void run_reachability_tests(int& passed, int& failed) {
    using namespace xfrontend;

    reach_case("Q001_empty_unit_has_no_graph", [&] {
        const auto result = reach_result("");
        reach_require(result.succeeded() && result.functions.empty(), "empty source has no functions");
    }, passed, failed);
    reach_case("Q002_declared_nonvoid_empty_body_missing_return", [&] {
        missing_return("function Read() returns int {}");
    }, passed, failed);
    reach_case("Q003_declared_nonvoid_fallthrough_missing_return", [&] {
        missing_return("function Read() returns int { PrintLine \"no value\"; }");
    }, passed, failed);
    reach_case("Q004_explicit_return_satisfies_contract", [&] {
        valid("function Read() returns int { return 5; }");
    }, passed, failed);
    reach_case("Q005_inferred_value_return_satisfies_contract", [&] {
        valid("function Read() { return 5; }");
    }, passed, failed);
    reach_case("Q006_inferred_return_with_if_fallthrough_is_rejected", [&] {
        missing_return("function Read(bool flag) { if (flag) { return 1; } }");
    }, passed, failed);
    reach_case("Q007_both_if_else_branches_return", [&] {
        valid("function Read(bool flag) returns int { if (flag) { return 1; } else { return 2; } }");
    }, passed, failed);
    reach_case("Q008_if_else_one_branch_falls_through", [&] {
        missing_return("function Read(bool flag) returns int { if (flag) { return 1; } else { PrintLine 2; } }");
    }, passed, failed);
    reach_case("Q009_nested_if_all_paths_return", [&] {
        valid("function Read(bool a, bool b) returns int { if (a) { if (b) { return 1; } else { return 2; } } else { return 3; } }");
    }, passed, failed);
    reach_case("Q010_nested_if_has_missing_path", [&] {
        missing_return("function Read(bool a, bool b) returns int { if (a) { if (b) { return 1; } } else { return 3; } }");
    }, passed, failed);
    reach_case("Q011_else_if_chain_is_exhaustive", [&] {
        valid("function Read(int value) returns int { if (value == 0) { return 0; } else if (value == 1) { return 1; } else { return 2; } }");
    }, passed, failed);
    reach_case("Q012_else_if_chain_without_final_else_falls_through", [&] {
        missing_return("function Read(int value) returns int { if (value == 0) { return 0; } else if (value == 1) { return 1; } }");
    }, passed, failed);
    reach_case("Q013_return_only_inside_unknown_while_may_fall_through", [&] {
        missing_return("function Read(bool flag) returns int { while (flag) { return 1; } }");
    }, passed, failed);
    reach_case("Q014_return_only_inside_for_may_fall_through", [&] {
        missing_return("function Read() returns int { for item in 0..3 { return item; } }");
    }, passed, failed);
    reach_case("Q015_literal_true_while_return_is_guaranteed", [&] {
        valid("function Read() returns int { while (true) { return 1; } }");
    }, passed, failed);
    reach_case("Q016_literal_true_infinite_loop_has_no_fallthrough", [&] {
        valid("function NeverReturns() returns int { while (true) { PrintLine \"working\"; } }");
    }, passed, failed);
    reach_case("Q017_literal_true_while_with_break_can_fall_through", [&] {
        missing_return("function Read() returns int { while (true) { break; } }");
    }, passed, failed);
    reach_case("Q018_literal_true_conditional_break_can_fall_through", [&] {
        missing_return("function Read(bool stop) returns int { while (true) { if (stop) { break; } else { return 1; } } }");
    }, passed, failed);
    reach_case("Q019_literal_false_while_body_does_not_return", [&] {
        missing_return("function Read() returns int { while (false) { return 1; } }");
    }, passed, failed);
    reach_case("Q020_literal_true_if_branch_is_guaranteed", [&] {
        valid("function Read() returns int { if (true) { return 1; } }");
    }, passed, failed);
    reach_case("Q021_literal_false_if_without_else_falls_through", [&] {
        missing_return("function Read() returns int { if (false) { return 1; } }");
    }, passed, failed);
    reach_case("Q022_literal_false_else_return_is_guaranteed", [&] {
        valid("function Read() returns int { if (false) { PrintLine 0; } else { return 1; } }");
    }, passed, failed);
    reach_case("Q023_literal_false_else_fallthrough_is_rejected", [&] {
        missing_return("function Read() returns int { if (false) { return 1; } else { PrintLine 0; } }");
    }, passed, failed);
    reach_case("Q024_return_followed_by_statements_is_not_fallthrough", [&] {
        valid("function Read() returns int { return 7; PrintLine \"dead\"; }");
    }, passed, failed);
    reach_case("Q025_graph_marks_reachable_return", [&] {
        const auto result = reach_result("function Read() returns int { return 7; }");
        reach_require(result.functions.size() == 1U && result.functions[0].has_reachable_return,
                      "reachable return marker expected");
    }, passed, failed);
    reach_case("Q026_graph_marks_reachable_fallthrough", [&] {
        const auto result = reach_result("function Read() { PrintLine 7; }");
        reach_require(result.functions.size() == 1U && result.functions[0].has_reachable_fallthrough,
                      "reachable fallthrough expected for void function");
    }, passed, failed);
    reach_case("Q027_returned_if_else_has_no_fallthrough", [&] {
        const auto result = reach_result("function Read(bool flag) returns int { if (flag) { return 1; } else { return 2; } }");
        reach_require(!result.functions[0].has_reachable_fallthrough, "all branches return");
    }, passed, failed);
    reach_case("Q028_literal_true_marks_else_unreachable", [&] {
        const auto result = reach_result("function Read() returns int { if (true) { return 1; } else { return 2; } }");
        reach_require(contains_unreachable(result.functions[0]), "else region should be unreachable");
    }, passed, failed);
    reach_case("Q029_literal_false_marks_then_unreachable", [&] {
        const auto result = reach_result("function Read() returns int { if (false) { return 1; } else { return 2; } }");
        reach_require(contains_unreachable(result.functions[0]), "then region should be unreachable");
    }, passed, failed);
    reach_case("Q030_literal_false_while_body_is_unreachable", [&] {
        const auto result = reach_result("function Read() { while (false) { PrintLine 1; } }");
        reach_require(contains_unreachable(result.functions[0]), "false loop body should be unreachable");
        reach_require(result.functions[0].has_reachable_fallthrough, "false loop falls through");
    }, passed, failed);
    reach_case("Q031_statement_after_return_is_represented_unreachable", [&] {
        const auto result = reach_result("function Read() returns int { return 1; int dead; PrintLine dead; }");
        reach_require(contains_unreachable(result.functions[0]), "trailing statement should have detached block");
        reach_require(result.succeeded(), "unreachable reads must not create flow errors");
    }, passed, failed);
    reach_case("Q032_statement_after_break_is_unreachable", [&] {
        const auto result = reach_result("function Read(bool flag) { while (flag) { break; PrintLine 1; } }");
        reach_require(contains_unreachable(result.functions[0]), "statement after break should be detached");
    }, passed, failed);
    reach_case("Q033_statement_after_continue_is_unreachable", [&] {
        const auto result = reach_result("function Read(bool flag) { while (flag) { continue; PrintLine 1; } }");
        reach_require(contains_unreachable(result.functions[0]), "statement after continue should be detached");
    }, passed, failed);
    reach_case("Q034_unreachable_uninitialized_read_is_not_reported", [&] {
        const auto result = reach_result("function Read() { return; int value; PrintLine value; }");
        reach_require(!has_code(result, "XFLOW0001"), "unreachable read is not an executable path");
    }, passed, failed);
    reach_case("Q035_unreachable_nested_conditional_is_recorded", [&] {
        const auto result = reach_result("function Read() returns int { return 1; if (true) { PrintLine 2; } }");
        reach_require(contains_unreachable(result.functions[0]), "nested unreachable conditional expected");
    }, passed, failed);
    reach_case("Q036_literal_true_loop_without_break_has_unreachable_after", [&] {
        const auto result = reach_result("function Never() { while (true) { PrintLine 1; } PrintLine 2; }");
        reach_require(contains_unreachable(result.functions[0]), "code after an infinite loop should be unreachable");
        reach_require(!result.functions[0].has_reachable_fallthrough, "infinite loop has no exit edge");
    }, passed, failed);
    reach_case("Q037_literal_true_loop_with_break_reaches_after", [&] {
        const auto result = reach_result("function Main() { while (true) { break; } PrintLine 2; }");
        reach_require(result.functions[0].has_reachable_fallthrough, "break should reach loop successor");
    }, passed, failed);
    reach_case("Q038_return_inside_nested_block_terminates", [&] {
        valid("function Read() returns int { { return 1; } }");
    }, passed, failed);
    reach_case("Q039_block_after_return_is_unreachable", [&] {
        const auto result = reach_result("function Read() returns int { return 1; { PrintLine 2; } }");
        reach_require(contains_unreachable(result.functions[0]), "nested block after return is unreachable");
    }, passed, failed);
    reach_case("Q040_void_function_may_fall_through", [&] {
        valid("function Run() returns void { PrintLine 1; }");
    }, passed, failed);
    reach_case("Q041_void_function_bare_return_is_valid", [&] {
        valid("function Run() returns void { return; }");
    }, passed, failed);
    reach_case("Q042_inferred_void_function_may_fall_through", [&] {
        valid("function Run() { PrintLine 1; }");
    }, passed, failed);
    reach_case("Q043_all_paths_return_before_unreachable_tail", [&] {
        valid("function Read(bool flag) returns int { if (flag) { return 1; } else { return 2; } PrintLine 3; }");
    }, passed, failed);
    reach_case("Q044_infinite_else_path_does_not_create_fallthrough", [&] {
        valid("function Read(bool flag) returns int { if (flag) { return 1; } else { while (true) { PrintLine 2; } } }");
    }, passed, failed);
    reach_case("Q045_constant_true_if_else_dead_branch_does_not_fallthrough", [&] {
        valid("function Read() returns int { if (true) { return 1; } else { PrintLine 2; } }");
    }, passed, failed);
    reach_case("Q046_constant_false_if_without_else_has_missing_return", [&] {
        missing_return("function Read() returns int { if (false) { return 1; } PrintLine 2; }");
    }, passed, failed);
    reach_case("Q047_missing_return_diagnostic_preserves_source_id", [&] {
        SourceManager sources;
        const SourceFileId id = sources.add_buffer("missing.src", "function Read() returns int { PrintLine 1; }");
        const auto result = analyze_control_flow(sources, id);
        const auto found = std::find_if(result.diagnostics.begin(), result.diagnostics.end(),
            [](const Diagnostic& item) { return item.code == "XFLOW0002"; });
        reach_require(found != result.diagnostics.end(), "missing-return diagnostic expected");
        reach_require(found->span.source_id == id, "diagnostic source identity must be preserved");
    }, passed, failed);
    reach_case("Q048_semantic_source_entry_runs_return_analysis", [&] {
        const auto result = analyze_source("function Read() returns int { PrintLine 1; }");
        reach_require(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
            [](const Diagnostic& item) { return item.code == "XFLOW0002"; }),
            "analyze_source must include missing-return diagnostics");
    }, passed, failed);
    reach_case("Q049_semantic_program_entry_runs_return_analysis", [&] {
        auto parsed = parse_source("module App; function Read() returns int { PrintLine 1; }");
        const auto result = analyze_program({ProgramFile{"app.src", 11U,
            std::make_shared<CompilationUnit>(std::move(parsed.unit))}});
        reach_require(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
            [](const Diagnostic& item) { return item.code == "XFLOW0002"; }),
            "analyze_program must include missing-return diagnostics");
    }, passed, failed);
    reach_case("Q050_unreachable_fallthrough_does_not_override_return", [&] {
        const auto result = reach_result("function Read() returns int { return 1; PrintLine 2; }");
        reach_require(!result.functions[0].has_reachable_fallthrough, "detached tail is unreachable");
        reach_require(!has_code(result, "XFLOW0002"), "no missing return on a reachable path");
    }, passed, failed);
    reach_case("Q051_unreachable_block_ids_are_in_range", [&] {
        const auto result = reach_result("function Run() { return; PrintLine 1; }");
        const auto& graph = result.functions[0];
        for (const auto id : graph.unreachable_blocks) {
            reach_require(id < graph.blocks.size(), "unreachable block id must be valid");
            reach_require(!graph.blocks[id].reachable, "unreachable list must match block metadata");
        }
    }, passed, failed);
    reach_case("Q052_every_block_reachability_flag_is_consistent", [&] {
        const auto result = reach_result("function Read(bool flag) returns int { if (flag) { return 1; } else { return 2; } PrintLine 3; }");
        const auto& graph = result.functions[0];
        for (const auto id : graph.unreachable_blocks) {
            reach_require(!graph.blocks[id].reachable, "unreachable block flag mismatch");
        }
        reach_require(std::any_of(graph.blocks.begin(), graph.blocks.end(),
            [](const BasicBlock& block) { return block.reachable; }), "entry path must be reachable");
    }, passed, failed);
}
