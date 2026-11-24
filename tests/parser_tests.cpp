// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/parser.hpp"
#include "test_report.hpp"

#include <chrono>

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require_parser(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

const xfrontend::FunctionDeclaration& first_function(const xfrontend::ParseResult& result) {
    require_parser(!result.unit.functions.empty(), "expected at least one function");
    return result.unit.functions.front();
}

const xfrontend::Statement& first_statement(const xfrontend::FunctionDeclaration& function) {
    require_parser(function.body != nullptr, "expected function body");
    require_parser(!function.body->statements.empty(), "expected at least one body statement");
    return *function.body->statements.front();
}

bool has_parse_diagnostic(const xfrontend::ParseResult& result) {
    return !result.diagnostics.empty();
}

void parser_case(const char* name, const std::function<void()>& body, int& passed, int& failed) {
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

void require_ok(const std::string& source) {
    const auto result = xfrontend::parse_source(source);
    require_parser(result.succeeded(), result.diagnostics.empty() ? "parse failed" : result.diagnostics.front().message);
}

void require_error(const std::string& source) {
    const auto result = xfrontend::parse_source(source);
    require_parser(has_parse_diagnostic(result), "expected syntax diagnostic");
}

} // namespace

void run_parser_tests(int& passed, int& failed) {
    using xfrontend::ExprKind;
    using xfrontend::StatementKind;
    using xfrontend::Visibility;

    parser_case("P001_empty_source", [&] {
        const auto result = xfrontend::parse_source("");
        require_parser(result.succeeded(), "empty source should parse");
        require_parser(result.unit.functions.empty(), "no functions expected");
    }, passed, failed);

    parser_case("P002_simple_function", [&] {
        const auto result = xfrontend::parse_source("function Start() {}");
        require_parser(result.succeeded(), "simple function should parse");
        require_parser(result.unit.functions.size() == 1, "one function expected");
        require_parser(first_function(result).name == "Start", "function name mismatch");
        require_parser(first_function(result).has_body, "body expected");
    }, passed, failed);

    parser_case("P003_typed_parameters", [&] {
        const auto result = xfrontend::parse_source("function Add(int left, int right) { return left + right; }");
        require_parser(result.succeeded(), "typed parameters should parse");
        require_parser(first_function(result).parameters.size() == 2, "two parameters expected");
        require_parser(first_function(result).parameters[0].type_name == "int", "parameter type mismatch");
        require_parser(first_function(result).parameters[1].name == "right", "parameter name mismatch");
    }, passed, failed);

    parser_case("P004_module_declaration", [&] {
        const auto result = xfrontend::parse_source("module Vendor.Device; function Start() {}");
        require_parser(result.succeeded(), "module declaration should parse");
        require_parser(result.unit.module_name == "Vendor.Device", "module path mismatch");
    }, passed, failed);

    parser_case("P005_multiple_imports", [&] {
        const auto result = xfrontend::parse_source("module App; import Vendor.IO; import Vendor.Math; function Start() {}");
        require_parser(result.succeeded(), "imports should parse");
        require_parser(result.unit.imports.size() == 2, "two imports expected");
        require_parser(result.unit.imports[1] == "Vendor.Math", "second import mismatch");
    }, passed, failed);

    parser_case("P006_public_visibility", [&] {
        const auto result = xfrontend::parse_source("public function Run() {}");
        require_parser(result.succeeded(), "public function should parse");
        require_parser(first_function(result).visibility == Visibility::Public, "public visibility expected");
    }, passed, failed);

    parser_case("P007_private_visibility", [&] {
        const auto result = xfrontend::parse_source("private function Run() {}");
        require_parser(result.succeeded(), "private function should parse");
        require_parser(first_function(result).visibility == Visibility::Private, "private visibility expected");
    }, passed, failed);

    parser_case("P008_internal_visibility", [&] {
        const auto result = xfrontend::parse_source("internal function Run() {}");
        require_parser(result.succeeded(), "internal function should parse");
        require_parser(first_function(result).visibility == Visibility::Internal, "internal visibility expected");
    }, passed, failed);

    parser_case("P009_declaration_without_body", [&] {
        const auto result = xfrontend::parse_source("public function Read() returns string;");
        require_parser(result.succeeded(), "function prototype should parse");
        require_parser(!first_function(result).has_body, "body must be absent");
        require_parser(first_function(result).return_type == "string", "return type mismatch");
    }, passed, failed);

    parser_case("P010_generic_function", [&] {
        const auto result = xfrontend::parse_source("function Max[T: Comparable](T left, T right) { return left; }");
        require_parser(result.succeeded(), "generic function should parse");
        require_parser(first_function(result).generic_parameters.size() == 1, "generic parameter expected");
        require_parser(first_function(result).generic_parameters[0] == "T:Comparable", "generic constraint mismatch");
    }, passed, failed);

    parser_case("P011_nested_generic_parameter_type", [&] {
        const auto result = xfrontend::parse_source("function Read(List[Map[string, int]] values) {}");
        require_parser(result.succeeded(), "nested generic type should parse");
        require_parser(first_function(result).parameters[0].type_name == "List[Map[string,int]]", "nested type mismatch");
    }, passed, failed);

    parser_case("P012_borrow_parameter", [&] {
        const auto result = xfrontend::parse_source("function Inspect(borrow *Device device) {}");
        require_parser(result.succeeded(), "borrow parameter should parse");
        require_parser(first_function(result).parameters[0].passing_mode == "borrow", "borrow mode expected");
        require_parser(first_function(result).parameters[0].type_name == "*Device", "pointer type expected");
    }, passed, failed);

    parser_case("P013_borrow_mut_parameter", [&] {
        const auto result = xfrontend::parse_source("function Update(borrow mut *Device device) {}");
        require_parser(result.succeeded(), "borrow mut parameter should parse");
        require_parser(first_function(result).parameters[0].passing_mode == "borrow mut", "borrow mut mode expected");
    }, passed, failed);

    parser_case("P014_take_parameter", [&] {
        const auto result = xfrontend::parse_source("function Consume(take Unique[Device] device) {}");
        require_parser(result.succeeded(), "take parameter should parse");
        require_parser(first_function(result).parameters[0].passing_mode == "take", "take mode expected");
        require_parser(first_function(result).parameters[0].type_name == "Unique[Device]", "take type mismatch");
    }, passed, failed);

    parser_case("P015_copy_parameter", [&] {
        const auto result = xfrontend::parse_source("function CopyValue(copy int value) {}");
        require_parser(result.succeeded(), "copy parameter should parse");
        require_parser(first_function(result).parameters[0].passing_mode == "copy", "copy mode expected");
    }, passed, failed);

    parser_case("P016_default_argument", [&] {
        const auto result = xfrontend::parse_source("function Connect(string host, int port = 80) {}");
        require_parser(result.succeeded(), "default argument should parse");
        require_parser(first_function(result).parameters[1].default_value != nullptr, "default expression expected");
        require_parser(first_function(result).parameters[1].default_value->text == "80", "default value mismatch");
    }, passed, failed);

    parser_case("P017_persian_string_in_body", [&] {
        const auto result = xfrontend::parse_source(u8"function Start() { PrintLine \"سلام دنیا\"; }");
        require_parser(result.succeeded(), "Persian literal should parse");
        require_parser(first_statement(first_function(result)).kind == StatementKind::Expression, "expression statement expected");
    }, passed, failed);

    parser_case("P018_bare_return", [&] {
        const auto result = xfrontend::parse_source("function Stop() { return; }");
        require_parser(result.succeeded(), "bare return should parse");
        require_parser(first_statement(first_function(result)).kind == StatementKind::Return, "return statement expected");
        require_parser(first_statement(first_function(result)).expressions.empty(), "bare return has no expression");
    }, passed, failed);

    parser_case("P019_return_expression", [&] {
        const auto result = xfrontend::parse_source("function Get() { return 42; }");
        require_parser(result.succeeded(), "return value should parse");
        require_parser(first_statement(first_function(result)).expressions.size() == 1, "return expression expected");
    }, passed, failed);

    parser_case("P020_explicit_local_declaration", [&] {
        const auto result = xfrontend::parse_source("function F() { int count = 1; }");
        require_parser(result.succeeded(), "typed local should parse");
        const auto& statement = first_statement(first_function(result));
        require_parser(statement.kind == StatementKind::VariableDeclaration, "variable declaration expected");
        require_parser(statement.name == "count" && statement.type_name == "int", "local declaration mismatch");
    }, passed, failed);

    parser_case("P021_typed_local_without_initializer", [&] {
        const auto result = xfrontend::parse_source("function F() { string name; }");
        require_parser(result.succeeded(), "uninitialized typed local should parse");
        require_parser(first_statement(first_function(result)).expressions.empty(), "initializer should be absent");
    }, passed, failed);

    parser_case("P022_implicit_assignment_statement", [&] {
        const auto result = xfrontend::parse_source("function F() { count = 10; }");
        require_parser(result.succeeded(), "implicit assignment should parse");
        require_parser(first_statement(first_function(result)).kind == StatementKind::Assignment, "assignment expected");
    }, passed, failed);

    parser_case("P023_multiple_statements", [&] {
        const auto result = xfrontend::parse_source("function F() { int x = 1; x = x + 1; return x; }");
        require_parser(result.succeeded(), "multiple statements should parse");
        require_parser(first_function(result).body->statements.size() == 3, "three statements expected");
    }, passed, failed);

    parser_case("P024_operator_precedence_multiplication", [&] {
        const auto result = xfrontend::parse_source("function F() { return 1 + 2 * 3; }");
        require_parser(result.succeeded(), "precedence expression should parse");
        const auto& expression = *first_statement(first_function(result)).expressions[0];
        require_parser(expression.kind == ExprKind::Binary && expression.text == "+", "addition must be root");
        require_parser(expression.children[1]->text == "*", "multiplication must bind tighter");
    }, passed, failed);

    parser_case("P025_parenthesized_expression", [&] {
        const auto result = xfrontend::parse_source("function F() { return (1 + 2) * 3; }");
        require_parser(result.succeeded(), "parenthesized expression should parse");
        const auto& expression = *first_statement(first_function(result)).expressions[0];
        require_parser(expression.text == "*", "multiplication should be root after parentheses");
    }, passed, failed);

    parser_case("P026_unary_expression", [&] {
        const auto result = xfrontend::parse_source("function F() { return -value; }");
        require_parser(result.succeeded(), "unary expression should parse");
        require_parser(first_statement(first_function(result)).expressions[0]->kind == ExprKind::Unary, "unary node expected");
    }, passed, failed);

    parser_case("P027_function_call", [&] {
        const auto result = xfrontend::parse_source("function F() { return Add(1, 2); }");
        require_parser(result.succeeded(), "function call should parse");
        require_parser(first_statement(first_function(result)).expressions[0]->kind == ExprKind::Call, "call node expected");
        require_parser(first_statement(first_function(result)).expressions[0]->children.size() == 3, "callee and two args expected");
    }, passed, failed);

    parser_case("P028_member_call", [&] {
        require_ok("function F() { return device.Read(); }");
    }, passed, failed);

    parser_case("P029_array_index", [&] {
        const auto result = xfrontend::parse_source("function F() { return values[2]; }");
        require_parser(result.succeeded(), "index expression should parse");
        require_parser(first_statement(first_function(result)).expressions[0]->kind == ExprKind::Index, "index node expected");
    }, passed, failed);

    parser_case("P030_list_literal", [&] {
        const auto result = xfrontend::parse_source("function F() { return [1, 2, 3]; }");
        require_parser(result.succeeded(), "list literal should parse");
        require_parser(first_statement(first_function(result)).expressions[0]->kind == ExprKind::List, "list node expected");
        require_parser(first_statement(first_function(result)).expressions[0]->children.size() == 3, "three elements expected");
    }, passed, failed);

    parser_case("P031_if_statement", [&] {
        const auto result = xfrontend::parse_source("function F() { if (ready) { return 1; } }");
        require_parser(result.succeeded(), "if statement should parse");
        require_parser(first_statement(first_function(result)).kind == StatementKind::If, "if node expected");
    }, passed, failed);

    parser_case("P032_if_else_statement", [&] {
        const auto result = xfrontend::parse_source("function F() { if (ready) { return 1; } else { return 0; } }");
        require_parser(result.succeeded(), "if else should parse");
        require_parser(first_statement(first_function(result)).else_branch != nullptr, "else block expected");
    }, passed, failed);

    parser_case("P033_else_if_chain", [&] {
        require_ok("function F() { if (a) { return 1; } else if (b) { return 2; } else { return 3; } }");
    }, passed, failed);

    parser_case("P034_while_loop", [&] {
        const auto result = xfrontend::parse_source("function F() { while (ready) { ready = false; } }");
        require_parser(result.succeeded(), "while loop should parse");
        require_parser(first_statement(first_function(result)).kind == StatementKind::While, "while node expected");
    }, passed, failed);

    parser_case("P035_for_in_loop", [&] {
        const auto result = xfrontend::parse_source("function F() { for item in items { PrintLine item; } }");
        require_parser(result.succeeded(), "for-in loop should parse");
        require_parser(first_statement(first_function(result)).kind == StatementKind::ForEach, "for each node expected");
        require_parser(first_statement(first_function(result)).name == "item", "iterator name mismatch");
    }, passed, failed);

    parser_case("P036_parenthesized_for_in_loop", [&] {
        require_ok("function F() { for (item in items) { continue; } }");
    }, passed, failed);

    parser_case("P037_break_and_continue", [&] {
        const auto result = xfrontend::parse_source("function F() { while (true) { break; continue; } }");
        require_parser(result.succeeded(), "break and continue should parse");
        const auto& loop = first_statement(first_function(result));
        require_parser(loop.statements[0]->statements.size() == 2, "two loop statements expected");
        require_parser(loop.statements[0]->statements[0]->kind == StatementKind::Break, "break expected");
        require_parser(loop.statements[0]->statements[1]->kind == StatementKind::Continue, "continue expected");
    }, passed, failed);

    parser_case("P038_nested_blocks", [&] {
        require_ok("function F() { { int x = 1; { x = 2; } } }");
    }, passed, failed);

    parser_case("P039_command_style_printline", [&] {
        const auto result = xfrontend::parse_source(u8"function F() { PrintLine \"متن\"; }");
        require_parser(result.succeeded(), "command-style call should parse");
        const auto& expr = *first_statement(first_function(result)).expressions[0];
        require_parser(expr.kind == ExprKind::Call, "call expression expected");
        require_parser(expr.children.size() == 2, "callee plus one argument expected");
    }, passed, failed);

    parser_case("P040_command_style_multiple_arguments", [&] {
        const auto result = xfrontend::parse_source("function F() { PrintLine first, second; }");
        require_parser(result.succeeded(), "multiple command arguments should parse");
        require_parser(first_statement(first_function(result)).expressions[0]->children.size() == 3, "two arguments expected");
    }, passed, failed);

    parser_case("P041_assignment_to_member", [&] {
        require_ok("function F() { config.Enabled = true; }");
    }, passed, failed);

    parser_case("P042_assignment_to_index", [&] {
        require_ok("function F() { values[0] = 9; }");
    }, passed, failed);

    parser_case("P043_chained_member_access", [&] {
        require_ok("function F() { return app.window.title; }");
    }, passed, failed);

    parser_case("P044_comparison_operators", [&] {
        require_ok("function F() { return left >= right; }");
    }, passed, failed);

    parser_case("P045_logical_operators", [&] {
        require_ok("function F() { return ready && (count > 0); }");
    }, passed, failed);

    parser_case("P046_bitwise_operators", [&] {
        require_ok("function F() { return flags | mask ^ bits & test; }");
    }, passed, failed);

    parser_case("P047_shift_operators", [&] {
        require_ok("function F() { return value << 2; }");
    }, passed, failed);

    parser_case("P048_decimal_and_exponent_literals", [&] {
        require_ok("function F() { return 12.5 + 6.02e23; }");
    }, passed, failed);

    parser_case("P049_boolean_and_null_literals", [&] {
        require_ok("function F() { bool a = true; object b = null; }");
    }, passed, failed);

    parser_case("P050_utf8_bom_and_persian_comment", [&] {
        const std::string source = std::string("\xEF\xBB\xBF") + u8"// توضیح\nfunction F() { PrintLine \"سلام\"; }";
        require_ok(source);
    }, passed, failed);

    parser_case("P051_case_insensitive_keywords", [&] {
        require_ok("FuNcTiOn F() { ReTuRn 1; }");
    }, passed, failed);

    parser_case("P052_dotted_module_name", [&] {
        const auto result = xfrontend::parse_source("module Company.Tools.Compiler; function F() {}");
        require_parser(result.succeeded(), "dotted module should parse");
        require_parser(result.unit.module_name == "Company.Tools.Compiler", "module path mismatch");
    }, passed, failed);

    parser_case("P053_comments_between_tokens", [&] {
        require_ok("function // gap\nF() { int value = 1; }");
    }, passed, failed);

    parser_case("P054_parameter_default_expression", [&] {
        const auto result = xfrontend::parse_source("function F(int value = 2 + 3) {}");
        require_parser(result.succeeded(), "default expression should parse");
        require_parser(first_function(result).parameters[0].default_value->kind == ExprKind::Binary, "binary default expected");
    }, passed, failed);

    parser_case("P055_function_span_byte_offsets", [&] {
        const auto result = xfrontend::parse_source(u8"// سلام\nfunction F() { return 1; }");
        require_parser(result.succeeded(), "source span test should parse");
        require_parser(first_function(result).span.start.line == 2, "function start line mismatch");
        require_parser(first_function(result).span.start.column == 1, "function start column mismatch");
    }, passed, failed);

    parser_case("P056_missing_module_semicolon", [&] { require_error("module App function F() {}"); }, passed, failed);
    parser_case("P057_missing_module_name", [&] { require_error("module ; function F() {}"); }, passed, failed);
    parser_case("P058_missing_import_semicolon", [&] { require_error("import IO function F() {}"); }, passed, failed);
    parser_case("P059_missing_function_name", [&] { require_error("function () {}"); }, passed, failed);
    parser_case("P060_missing_function_open_paren", [&] { require_error("function F {}"); }, passed, failed);
    parser_case("P061_missing_parameter_type", [&] { require_error("function F(value) {}"); }, passed, failed);
    parser_case("P062_missing_parameter_name", [&] { require_error("function F(int) {}"); }, passed, failed);
    parser_case("P063_missing_parameter_close_paren", [&] { require_error("function F(int x { }"); }, passed, failed);
    parser_case("P064_missing_function_body_or_semicolon", [&] { require_error("function F()"); }, passed, failed);
    parser_case("P065_missing_return_type", [&] { require_error("function F() returns {}"); }, passed, failed);
    parser_case("P066_return_missing_semicolon", [&] { require_error("function F() { return 1 }"); }, passed, failed);
    parser_case("P067_assignment_missing_value", [&] { require_error("function F() { x = ; }"); }, passed, failed);
    parser_case("P068_type_name_can_be_an_identifier_in_assignment", [&] {
        const auto result = xfrontend::parse_source("function F() { int = 1; }");
        require_parser(result.succeeded(), "parser leaves identifier/type ambiguity to semantics");
        require_parser(first_statement(first_function(result)).kind == StatementKind::Assignment, "assignment expected");
    }, passed, failed);
    parser_case("P069_variable_missing_semicolon", [&] { require_error("function F() { int x = 1 }"); }, passed, failed);
    parser_case("P070_if_missing_open_paren", [&] { require_error("function F() { if ready { } }"); }, passed, failed);
    parser_case("P071_if_missing_close_paren", [&] { require_error("function F() { if (ready { } }"); }, passed, failed);
    parser_case("P072_if_missing_block", [&] { require_error("function F() { if (ready) return; }"); }, passed, failed);
    parser_case("P073_while_missing_condition", [&] { require_error("function F() { while () { } }"); }, passed, failed);
    parser_case("P074_for_missing_iterator", [&] { require_error("function F() { for in items { } }"); }, passed, failed);
    parser_case("P075_for_missing_in", [&] { require_error("function F() { for item items { } }"); }, passed, failed);
    parser_case("P076_for_missing_body", [&] { require_error("function F() { for item in items; }"); }, passed, failed);
    parser_case("P077_missing_block_close", [&] { require_error("function F() { return 1;"); }, passed, failed);
    parser_case("P078_missing_expression_semicolon", [&] { require_error("function F() { Call() }"); }, passed, failed);
    parser_case("P079_missing_call_close_paren", [&] { require_error("function F() { Call(1; }"); }, passed, failed);
    parser_case("P080_missing_member_name", [&] { require_error("function F() { return value.; }"); }, passed, failed);
    parser_case("P081_missing_index_expression", [&] { require_error("function F() { return value[]; }"); }, passed, failed);
    parser_case("P082_missing_index_close", [&] { require_error("function F() { return value[0; }"); }, passed, failed);
    parser_case("P083_missing_binary_rhs", [&] { require_error("function F() { return 1 +; }"); }, passed, failed);
    parser_case("P084_missing_unary_operand", [&] { require_error("function F() { return -; }"); }, passed, failed);
    parser_case("P085_missing_list_close", [&] { require_error("function F() { return [1, 2; }"); }, passed, failed);
    parser_case("P086_import_after_function", [&] { require_error("function F() {} import IO;"); }, passed, failed);
    parser_case("P087_module_after_function", [&] { require_error("function F() {} module App;"); }, passed, failed);
    parser_case("P088_unexpected_top_level_statement", [&] { require_error("x = 1;"); }, passed, failed);
    parser_case("P089_multiple_functions", [&] {
        const auto result = xfrontend::parse_source("function A() {} public function B() {}");
        require_parser(result.succeeded(), "multiple functions should parse");
        require_parser(result.unit.functions.size() == 2, "two functions expected");
    }, passed, failed);

    parser_case("P090_assignment_expression_precedence", [&] {
        const auto result = xfrontend::parse_source("function F() { x = a + b * c; }");
        require_parser(result.succeeded(), "assignment with precedence should parse");
        const auto& value = *first_statement(first_function(result)).expressions[1];
        require_parser(value.text == "+" && value.children[1]->text == "*", "operator precedence tree mismatch");
    }, passed, failed);

    parser_case("P091_empty_parameter_list", [&] { require_ok("function F() {}"); }, passed, failed);
    parser_case("P092_empty_list_literal", [&] { require_ok("function F() { return []; }"); }, passed, failed);
    parser_case("P093_nested_call_expression", [&] { require_ok("function F() { return Outer(Inner(1)); }"); }, passed, failed);
    parser_case("P094_nested_member_and_index", [&] { require_ok("function F() { return app.items[0].name; }"); }, passed, failed);
    parser_case("P095_nested_if_while", [&] { require_ok("function F() { if (ready) { while (active) { break; } } }"); }, passed, failed);
    parser_case("P096_multiple_diagnostics", [&] {
        const auto result = xfrontend::parse_source("function F() { int x = ; return 1 }");
        require_parser(result.diagnostics.size() >= 2, "multiple syntax diagnostics expected");
    }, passed, failed);

    parser_case("P097_numeric_range_loop", [&] {
        const auto result = xfrontend::parse_source("function F() { for i in 0 .. 10 { PrintLine i; } }");
        require_parser(result.succeeded(), "numeric range for loop should parse");
        const auto& range = *first_statement(first_function(result)).expressions[0];
        require_parser(range.kind == ExprKind::Binary && range.text == "..", "range expression node expected");
    }, passed, failed);

    parser_case("P098_range_expression_in_return", [&] {
        const auto result = xfrontend::parse_source("function F() { return 1 .. 5; }");
        require_parser(result.succeeded(), "range expression should parse");
        require_parser(first_statement(first_function(result)).expressions[0]->text == "..", "range operator expected");
    }, passed, failed);

    parser_case("P099_left_associative_subtraction", [&] {
        const auto result = xfrontend::parse_source("function F() { return 10 - 3 - 2; }");
        require_parser(result.succeeded(), "subtraction chain should parse");
        const auto& expression = *first_statement(first_function(result)).expressions[0];
        require_parser(expression.text == "-" && expression.children[0]->text == "-", "subtraction must be left associative");
    }, passed, failed);

    parser_case("P100_diagnostic_has_source_location", [&] {
        const auto result = xfrontend::parse_source("function F() { return 1 }");
        require_parser(!result.succeeded(), "missing semicolon should be diagnosed");
        require_parser(result.diagnostics[0].span.start.line == 1, "diagnostic line should be one");
        require_parser(result.diagnostics[0].span.start.column > 1, "diagnostic column should be populated");
    }, passed, failed);

}
