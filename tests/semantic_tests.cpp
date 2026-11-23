#include "xfrontend/semantic.hpp"
#include "xfrontend/source_manager.hpp"
#include "test_report.hpp"

#include <chrono>

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void semantic_require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

xfrontend::SemanticResult semantic_result(const std::string& source) {
    const auto parsed = xfrontend::parse_source(source);
    if (!parsed.succeeded()) {
        throw std::runtime_error("test source failed to parse: " + parsed.diagnostics.front().code +
                                 " " + parsed.diagnostics.front().message);
    }
    return xfrontend::analyze_semantics(parsed.unit);
}

bool has_code(const xfrontend::SemanticResult& result, const std::string& code) {
    for (const auto& item : result.diagnostics) {
        if (item.code == code) return true;
    }
    return false;
}

void semantic_case(const char* name, const std::function<void()>& body,
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
    const auto result = semantic_result(source);
    semantic_require(result.succeeded(), "unexpected semantic diagnostics: " +
                     (result.diagnostics.empty() ? std::string() : result.diagnostics.front().code +
                      " " + result.diagnostics.front().message));
}

void invalid(const std::string& source, const std::string& code) {
    const auto result = semantic_result(source);
    semantic_require(!result.succeeded(), "expected a semantic diagnostic");
    semantic_require(has_code(result, code), "expected diagnostic " + code);
}

} // namespace

void run_semantic_tests(int& passed, int& failed) {
    semantic_case("A001_empty_unit", [&] { valid(""); }, passed, failed);
    semantic_case("A002_simple_void_function", [&] { valid("function Start() { PrintLine \"ok\"; }"); }, passed, failed);
    semantic_case("A003_infer_integer_return", [&] {
        const auto result = semantic_result("function Value() { return 42; }");
        semantic_require(result.succeeded(), "valid return should pass");
        semantic_require(result.functions.size() == 1U, "function symbol should be published");
        semantic_require(result.functions[0].inferred_return_type == "int", "int return should be inferred");
    }, passed, failed);
    semantic_case("A004_infer_string_return", [&] {
        const auto result = semantic_result(u8"function Text() { return \"سلام\"; }");
        semantic_require(result.succeeded(), "valid string return should pass");
        semantic_require(result.functions[0].inferred_return_type == "string", "string return should be inferred");
    }, passed, failed);
    semantic_case("A005_infer_boolean_return", [&] {
        const auto result = semantic_result("function Flag() { return true; }");
        semantic_require(result.functions[0].inferred_return_type == "bool", "bool return should be inferred");
    }, passed, failed);
    semantic_case("A006_infer_float_return", [&] {
        const auto result = semantic_result("function Value() { return 1.25; }");
        semantic_require(result.functions[0].inferred_return_type == "float", "float return should be inferred");
    }, passed, failed);
    semantic_case("A007_parameter_reference", [&] {
        valid("function Add(int left, int right) returns int { return left + right; }");
    }, passed, failed);
    semantic_case("A008_case_insensitive_parameter_reference", [&] {
        valid("function Echo(string value) returns string { return VALUE; }");
    }, passed, failed);
    semantic_case("A009_duplicate_parameter", [&] {
        invalid("function Bad(int value, string value) { return; }", "XSEM0004");
    }, passed, failed);
    semantic_case("A010_duplicate_parameter_casefold", [&] {
        invalid("function Bad(int Value, int value) { return; }", "XSEM0004");
    }, passed, failed);
    semantic_case("A011_duplicate_exact_function_signature", [&] {
        invalid("function Same(int value) { return; } function Same(int other) { return; }", "XSEM0001");
    }, passed, failed);
    semantic_case("A012_same_name_different_overload_types", [&] {
        valid("function Same(int value) returns int { return value; } function Same(string value) returns string { return value; }");
    }, passed, failed);
    semantic_case("A013_function_name_case_insensitive", [&] {
        valid("function Sum(int x) returns int { return x; } function Main() { int y = sUm(1); }");
    }, passed, failed);
    semantic_case("A014_call_exact_overload", [&] {
        valid("function Pick(int x) returns int { return x; } function Pick(string x) returns string { return x; } function Main() { int n = Pick(2); }");
    }, passed, failed);
    semantic_case("A015_call_accepts_default_argument", [&] {
        valid("function Add(int x, int y = 2) returns int { return x + y; } function Main() { int n = Add(3); }");
    }, passed, failed);
    semantic_case("A016_call_no_overload_for_arity", [&] {
        invalid("function One(int x) { return; } function Main() { One(); }", "XSEM0032");
    }, passed, failed);
    semantic_case("A017_call_no_overload_for_argument_type", [&] {
        invalid("function One(int x) { return; } function Main() { One(\"text\"); }", "XSEM0033");
    }, passed, failed);
    semantic_case("A018_declaration_without_body_is_symbol", [&] {
        valid("function External(int x) returns int; function Main() { External(1); }");
    }, passed, failed);
    semantic_case("A019_duplicate_local_same_scope", [&] {
        invalid("function Main() { int value = 1; string value = \"x\"; }", "XSEM0002");
    }, passed, failed);
    semantic_case("A020_duplicate_local_casefold", [&] {
        invalid("function Main() { int Value = 1; int value = 2; }", "XSEM0002");
    }, passed, failed);
    semantic_case("A021_explicit_local_initializer_match", [&] {
        valid("function Main() { int count = 3; string name = \"x\"; bool ready = true; }");
    }, passed, failed);
    semantic_case("A022_explicit_local_initializer_mismatch", [&] {
        invalid("function Main() { int count = \"three\"; }", "XSEM0013");
    }, passed, failed);
    semantic_case("A023_unresolved_identifier", [&] {
        invalid("function Main() { PrintLine missing; }", "XSEM0008");
    }, passed, failed);
    semantic_case("A024_unresolved_assignment_rhs", [&] {
        invalid("function Main() { int count = unknown; }", "XSEM0008");
    }, passed, failed);
    semantic_case("A025_first_assignment_creates_implicit_local", [&] {
        valid("function Main() { total = 4; total = total + 1; }");
    }, passed, failed);
    semantic_case("A026_implicit_local_type_is_checked", [&] {
        invalid("function Main() { total = 4; total = \"four\"; }", "XSEM0014");
    }, passed, failed);
    semantic_case("A027_explicit_assignment_type_match", [&] {
        valid("function Main() { int count = 1; count = 2; }");
    }, passed, failed);
    semantic_case("A028_explicit_assignment_type_mismatch", [&] {
        invalid("function Main() { int count = 1; count = \"two\"; }", "XSEM0014");
    }, passed, failed);
    semantic_case("A029_integer_promotes_to_float", [&] {
        valid("function Main() { float value = 2; }");
    }, passed, failed);
    semantic_case("A030_float_does_not_promote_to_int", [&] {
        invalid("function Main() { int value = 2.5; }", "XSEM0013");
    }, passed, failed);
    semantic_case("A031_nested_block_reads_outer_local", [&] {
        valid("function Main() { int count = 1; if (true) { count = count + 1; } }");
    }, passed, failed);
    semantic_case("A032_implicit_child_local_stays_in_child_scope", [&] {
        invalid("function Main() { if (true) { child = 1; } PrintLine child; }", "XSEM0008");
    }, passed, failed);
    semantic_case("A033_explicit_shadowing_is_deferred", [&] {
        invalid("function Main() { int value = 1; if (true) { int value = 2; } }", "XSEM0003");
    }, passed, failed);
    semantic_case("A034_sibling_blocks_have_independent_scopes", [&] {
        valid("function Main() { if (true) { int local = 1; } if (true) { string local = \"x\"; } }");
    }, passed, failed);
    semantic_case("A035_nested_block_reads_parameter", [&] {
        valid("function Main(int value) { while (true) { PrintLine value; break; } }");
    }, passed, failed);
    semantic_case("A036_infer_numeric_branch_return", [&] {
        const auto result = semantic_result("function Value(bool flag) { if (flag) { return 1; } else { return 2.5; } }");
        semantic_require(result.succeeded(), "compatible numeric return paths should pass");
        semantic_require(result.functions[0].inferred_return_type == "float", "numeric return should promote to float");
    }, passed, failed);
    semantic_case("A037_incompatible_return_categories", [&] {
        invalid("function Value(bool flag) { if (flag) { return 1; } else { return \"x\"; } }", "XSEM0010");
    }, passed, failed);
    semantic_case("A038_mixed_bare_and_value_return", [&] {
        invalid("function Value(bool flag) { if (flag) { return 1; } else { return; } }", "XSEM0011");
    }, passed, failed);
    semantic_case("A039_explicit_return_type_match", [&] {
        valid("function Value() returns int { return 12; }");
    }, passed, failed);
    semantic_case("A040_explicit_return_type_mismatch", [&] {
        invalid("function Value() returns int { return \"twelve\"; }", "XSEM0012");
    }, passed, failed);
    semantic_case("A041_void_function_bare_return", [&] {
        valid("function Stop() { return; }");
    }, passed, failed);
    semantic_case("A042_void_declared_function_value_return", [&] {
        invalid("function Stop() returns void { return 1; }", "XSEM0012");
    }, passed, failed);
    semantic_case("A043_if_boolean_condition", [&] {
        valid("function Main() { if (true) { PrintLine \"yes\"; } }");
    }, passed, failed);
    semantic_case("A044_if_non_boolean_condition", [&] {
        invalid("function Main() { if (1) { PrintLine \"yes\"; } }", "XSEM0018");
    }, passed, failed);
    semantic_case("A045_while_boolean_condition", [&] {
        valid("function Main(bool run) { while (run) { break; } }");
    }, passed, failed);
    semantic_case("A046_while_non_boolean_condition", [&] {
        invalid("function Main() { while (3) { break; } }", "XSEM0018");
    }, passed, failed);
    semantic_case("A047_logical_operators_boolean", [&] {
        valid("function Main(bool a, bool b) { bool value = a && !b; }");
    }, passed, failed);
    semantic_case("A048_logical_operators_reject_integer", [&] {
        invalid("function Main() { bool value = 1 && true; }", "XSEM0026");
    }, passed, failed);
    semantic_case("A049_arithmetic_integer", [&] {
        valid("function Main() { int value = 2 + 3 * 4; }");
    }, passed, failed);
    semantic_case("A050_arithmetic_rejects_string", [&] {
        invalid("function Main() { int value = \"a\" - \"b\"; }", "XSEM0029");
    }, passed, failed);
    semantic_case("A051_string_concatenation", [&] {
        valid("function Main() { string value = \"a\" + \"b\"; }");
    }, passed, failed);
    semantic_case("A052_comparison_returns_bool", [&] {
        valid("function Main() { bool ok = 1 < 2; }");
    }, passed, failed);
    semantic_case("A053_comparison_rejects_incompatible_types", [&] {
        invalid("function Main() { bool ok = 1 < \"2\"; }", "XSEM0027");
    }, passed, failed);
    semantic_case("A054_bitwise_integer", [&] {
        valid("function Main() { int mask = 3 & 1; }");
    }, passed, failed);
    semantic_case("A055_bitwise_rejects_float", [&] {
        invalid("function Main() { int mask = 3.5 & 1; }", "XSEM0028");
    }, passed, failed);
    semantic_case("A056_unary_negation_boolean", [&] {
        valid("function Main(bool ready) { bool off = !ready; }");
    }, passed, failed);
    semantic_case("A057_unary_negation_rejects_string", [&] {
        invalid("function Main() { bool off = !\"ready\"; }", "XSEM0019");
    }, passed, failed);
    semantic_case("A058_for_range_integer", [&] {
        valid("function Main() { for index in 0..10 { int next = index + 1; } }");
    }, passed, failed);
    semantic_case("A059_for_range_rejects_string_endpoint", [&] {
        invalid("function Main() { for index in 0..\"ten\" { PrintLine index; } }", "XSEM0024");
    }, passed, failed);
    semantic_case("A060_for_iterator_is_visible_in_body", [&] {
        valid("function Main() { for item in [1, 2, 3] { PrintLine item; } }");
    }, passed, failed);
    semantic_case("A061_for_iterator_not_visible_after_loop", [&] {
        invalid("function Main() { for item in [1, 2] { PrintLine item; } PrintLine item; }", "XSEM0008");
    }, passed, failed);
    semantic_case("A062_break_outside_loop", [&] {
        invalid("function Main() { break; }", "XSEM0017");
    }, passed, failed);
    semantic_case("A063_continue_outside_loop", [&] {
        invalid("function Main() { continue; }", "XSEM0017");
    }, passed, failed);
    semantic_case("A064_break_inside_loop", [&] {
        valid("function Main() { while (true) { break; } }");
    }, passed, failed);
    semantic_case("A065_continue_inside_nested_loop", [&] {
        valid("function Main() { while (true) { for item in [1] { continue; } break; } }");
    }, passed, failed);
    semantic_case("A066_homogeneous_list_literal", [&] {
        valid("function Main() { List[int] values = [1, 2, 3]; }");
    }, passed, failed);
    semantic_case("A067_mixed_list_literal", [&] {
        invalid("function Main() { List[int] values = [1, \"two\"]; }", "XSEM0023");
    }, passed, failed);
    semantic_case("A068_list_index_integer", [&] {
        valid("function Main() { List[int] values = [1, 2]; int first = values[0]; }");
    }, passed, failed);
    semantic_case("A069_list_index_rejects_boolean", [&] {
        invalid("function Main() { List[int] values = [1, 2]; int first = values[true]; }", "XSEM0022");
    }, passed, failed);
    semantic_case("A070_imported_namespace_call_is_deferred_to_linker", [&] {
        valid("module App; import Vendor.Device; function Main() { Vendor.Device.Open(); }");
    }, passed, failed);
    semantic_case("A071_printline_builtin", [&] {
        valid("function Main() { PrintLine \"hello\", 5; }");
    }, passed, failed);
    semantic_case("A072_input_builtin_returns_string", [&] {
        valid("function Main() { string value = Input(); }");
    }, passed, failed);
    semantic_case("A073_variable_is_not_callable", [&] {
        invalid("function Main() { int action = 1; action(); }", "XSEM0031");
    }, passed, failed);
    semantic_case("A074_module_namespace_is_not_assignable", [&] {
        invalid("module App; import Vendor.IO; function Main() { Vendor = 4; }", "XSEM0016");
    }, passed, failed);
    semantic_case("A075_user_defined_type_is_preserved", [&] {
        valid("function Main() { Customer value; }");
    }, passed, failed);
    semantic_case("A076_semantic_diagnostic_preserves_source_id", [&] {
        xfrontend::SourceManager sources;
        const auto id = sources.add_buffer("semantic.src", "function Main() { PrintLine missing; }");
        const auto result = xfrontend::analyze_source(sources, id);
        semantic_require(!result.diagnostics.empty(), "diagnostic expected");
        semantic_require(result.diagnostics[0].span.source_id == id, "source id should be preserved");
        semantic_require(result.diagnostics[0].span.start.line == 1U, "source line should be preserved");
    }, passed, failed);
    semantic_case("A077_source_parse_diagnostic_is_propagated", [&] {
        xfrontend::SourceManager sources;
        const auto id = sources.add_buffer("bad-semantic.src", "function Broken( {");
        const auto result = xfrontend::analyze_source(sources, id);
        semantic_require(!result.succeeded(), "parse error should stop semantic analysis");
        semantic_require(!result.diagnostics.empty(), "parse diagnostic expected");
        semantic_require(result.diagnostics[0].code.find("XPARSE") == 0U,
                         "parser diagnostic should be preserved");
        semantic_require(result.diagnostics[0].span.source_id == id,
                         "parser diagnostic should preserve source id");
    }, passed, failed);
}
