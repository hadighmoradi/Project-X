// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/semantic.hpp"
#include "xfrontend/source_manager.hpp"
#include "test_report.hpp"

#include <chrono>

#include <algorithm>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void module_require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void module_case(const char* name, const std::function<void()>& body,
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

xfrontend::ProgramFile program_file(const std::string& name, const std::string& source,
                                    xfrontend::SourceFileId id = xfrontend::InvalidSourceFileId) {
    auto parsed = xfrontend::parse_source(source);
    if (!parsed.succeeded()) {
        throw std::runtime_error("module test source failed to parse: " +
            parsed.diagnostics.front().code + " " + parsed.diagnostics.front().message);
    }
    xfrontend::ProgramFile file;
    file.display_name = name;
    file.source_id = id;
    file.unit = std::make_shared<xfrontend::CompilationUnit>(std::move(parsed.unit));
    return file;
}

bool has_module_code(const xfrontend::ModuleResolutionResult& result, const std::string& code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto& item) { return item.code == code; });
}

bool has_semantic_code(const xfrontend::SemanticResult& result, const std::string& code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto& item) { return item.code == code; });
}

} // namespace

void run_module_tests(int& passed, int& failed) {
    using xfrontend::ProgramFile;
    using xfrontend::resolve_modules;

    module_case("R001_single_module_summary", [&] {
        const auto result = resolve_modules({program_file("core.src",
            "module Core; public function Start() { }")});
        module_require(result.succeeded(), "single module should resolve");
        module_require(result.modules.size() == 1U, "one module summary expected");
        module_require(result.modules[0].name == "Core", "module spelling should be preserved");
        module_require(result.modules[0].public_function_signatures.size() == 1U,
                       "one public function expected");
    }, passed, failed);

    module_case("R002_missing_module_header", [&] {
        const auto result = resolve_modules({program_file("plain.src", "function Main() {}")});
        module_require(has_module_code(result, "XMOD0001"), "module header diagnostic expected");
    }, passed, failed);

    module_case("R003_unknown_import", [&] {
        const auto result = resolve_modules({program_file("app.src",
            "module App; import Missing.Core; function Main() {}")});
        module_require(has_module_code(result, "XMOD0003"), "unknown import diagnostic expected");
    }, passed, failed);

    module_case("R004_case_insensitive_module_import", [&] {
        const auto result = resolve_modules({
            program_file("core.src", "module Vendor.Device; public function Open() {}"),
            program_file("app.src", "module App; import vendor.device; function Main() { Vendor.Device.Open(); }")});
        module_require(result.succeeded(), "module/import names should be case-insensitive");
    }, passed, failed);

    module_case("R005_same_module_multiple_files", [&] {
        const auto result = resolve_modules({
            program_file("one.src", "module Shared; function First() {}"),
            program_file("two.src", "module shared; function Second() {}")});
        module_require(result.succeeded(), "same module may span files");
        module_require(result.modules.size() == 1U, "files should be grouped into one module");
    }, passed, failed);

    module_case("R006_duplicate_signature_same_file", [&] {
        const auto result = resolve_modules({program_file("dup.src",
            "module Core; public function Run(int x) {} public function Run(int y) {}")});
        module_require(has_module_code(result, "XMOD0002"), "duplicate signature expected");
    }, passed, failed);

    module_case("R007_duplicate_signature_across_files", [&] {
        const auto result = resolve_modules({
            program_file("a.src", "module Core; public function Run(int x) {}"),
            program_file("b.src", "module Core; public function Run(int y) {}")});
        module_require(has_module_code(result, "XMOD0002"), "cross-file duplicate expected");
    }, passed, failed);

    module_case("R008_prototype_definition_pair_allowed", [&] {
        const auto result = resolve_modules({
            program_file("decl.src", "module Core; public function Run(int x) returns int;"),
            program_file("impl.src", "module Core; public function Run(int value) returns int { return value; }")});
        module_require(result.succeeded(), "matching prototype/definition pair should be accepted");
    }, passed, failed);

    module_case("R009_visibility_mismatch_prototype_pair_rejected", [&] {
        const auto result = resolve_modules({
            program_file("decl.src", "module Core; function Run(int x) returns int;"),
            program_file("impl.src", "module Core; public function Run(int value) returns int { return value; }")});
        module_require(has_module_code(result, "XMOD0002"), "visibility mismatch must be rejected");
    }, passed, failed);

    module_case("R010_two_module_cycle", [&] {
        const auto result = resolve_modules({
            program_file("a.src", "module A; import B; function A1() {}"),
            program_file("b.src", "module B; import A; function B1() {}")});
        module_require(has_module_code(result, "XMOD0004"), "two-node cycle expected");
    }, passed, failed);

    module_case("R011_self_import_cycle", [&] {
        const auto result = resolve_modules({program_file("a.src",
            "module A; import A; function A1() {}")});
        module_require(has_module_code(result, "XMOD0004"), "self-cycle expected");
    }, passed, failed);

    module_case("R012_three_module_cycle", [&] {
        const auto result = resolve_modules({
            program_file("a.src", "module A; import B; function A1() {}"),
            program_file("b.src", "module B; import C; function B1() {}"),
            program_file("c.src", "module C; import A; function C1() {}")});
        module_require(has_module_code(result, "XMOD0004"), "three-node cycle expected");
    }, passed, failed);

    module_case("R013_acyclic_dependency_chain", [&] {
        const auto result = resolve_modules({
            program_file("a.src", "module A; import B; function A1() {}"),
            program_file("b.src", "module B; import C; function B1() {}"),
            program_file("c.src", "module C; function C1() {}")});
        module_require(result.succeeded(), "DAG dependency graph should resolve");
    }, passed, failed);

    module_case("R014_public_imported_call", [&] {
        const auto result = resolve_modules({
            program_file("device.src", "module Vendor.Device; public function Open() returns bool { return true; }"),
            program_file("app.src", "module App; import Vendor.Device; function Main() returns bool { return Vendor.Device.Open(); }")});
        module_require(result.succeeded(), "public imported call should resolve");
    }, passed, failed);

    module_case("R015_private_imported_call_rejected", [&] {
        const auto result = resolve_modules({
            program_file("device.src", "module Vendor.Device; function Hidden() {}"),
            program_file("app.src", "module App; import Vendor.Device; function Main() { Vendor.Device.Hidden(); }")});
        module_require(has_module_code(result, "XMOD0006"), "private export diagnostic expected");
    }, passed, failed);

    module_case("R016_unknown_exported_function", [&] {
        const auto result = resolve_modules({
            program_file("device.src", "module Vendor.Device; public function Open() {}"),
            program_file("app.src", "module App; import Vendor.Device; function Main() { Vendor.Device.Close(); }")});
        module_require(has_module_code(result, "XMOD0007"), "missing exported function expected");
    }, passed, failed);

    module_case("R017_qualified_call_without_import", [&] {
        const auto result = resolve_modules({
            program_file("device.src", "module Vendor.Device; public function Open() {}"),
            program_file("app.src", "module App; function Main() { Vendor.Device.Open(); }")});
        module_require(has_module_code(result, "XMOD0011"), "known module must be imported before use");
    }, passed, failed);

    module_case("R018_wrong_qualified_import_path", [&] {
        const auto result = resolve_modules({
            program_file("device.src", "module Vendor.Device; public function Open() {}"),
            program_file("other.src", "module Vendor.Other; public function Run() {}"),
            program_file("app.src", "module App; import Vendor.Device; function Main() { Vendor.Other.Run(); }")});
        module_require(has_module_code(result, "XMOD0005") || has_module_code(result, "XMOD0011"), "wrong import path must be rejected");
    }, passed, failed);

    module_case("R019_call_wrong_arity", [&] {
        const auto result = resolve_modules({
            program_file("api.src", "module Api; public function Send(int code, string text) {}"),
            program_file("app.src", "module App; import Api; function Main() { Api.Send(4); }")});
        module_require(has_module_code(result, "XMOD0008"), "wrong arity must be diagnosed");
    }, passed, failed);

    module_case("R020_overload_by_distinct_arity", [&] {
        const auto result = resolve_modules({
            program_file("api.src", "module Api; public function Send() {} public function Send(int code, string text) {}"),
            program_file("app.src", "module App; import Api; function Main() { Api.Send(); Api.Send(4, \"ok\"); }")});
        module_require(result.succeeded(), "distinct arity overloads should resolve");
    }, passed, failed);

    module_case("R021_unknown_argument_overload_is_deferred_to_semantics", [&] {
        const auto result = resolve_modules({
            program_file("api.src", "module Api; public function Send(int code) {} public function Send(string text) {}"),
            program_file("app.src", "module App; import Api; function Main(string unknown) { Api.Send(unknown); }")});
        module_require(!has_module_code(result, "XMOD0009"),
                       "resolver must defer unknown identifier types to SemanticAnalyzer");
    }, passed, failed);

    module_case("R022_duplicate_import_is_idempotent", [&] {
        const auto result = resolve_modules({
            program_file("api.src", "module Api; public function Send() {}"),
            program_file("app.src", "module App; import Api; import api; function Main() { Api.Send(); }")});
        module_require(result.succeeded(), "duplicate import should be idempotent");
    }, passed, failed);

    module_case("R023_public_summary_excludes_private", [&] {
        const auto result = resolve_modules({program_file("api.src",
            "module Api; function Hidden() {} public function External() {}")});
        module_require(result.succeeded(), "module should resolve");
        module_require(result.modules[0].public_function_signatures.size() == 1U,
                       "only public functions should be exported");
    }, passed, failed);

    module_case("R024_empty_export_module_is_valid", [&] {
        const auto result = resolve_modules({program_file("empty.src", "module Empty;")});
        module_require(result.succeeded(), "module with no exports should be valid");
        module_require(result.modules.size() == 1U, "empty module summary expected");
    }, passed, failed);

    module_case("R025_dotted_module_path", [&] {
        const auto result = resolve_modules({
            program_file("api.src", "module Vendor.Device.Input; public function Read() {}"),
            program_file("app.src", "module App; import Vendor.Device.Input; function Main() { Vendor.Device.Input.Read(); }")});
        module_require(result.succeeded(), "three-segment module path should resolve");
    }, passed, failed);

    module_case("R026_import_diagnostic_source_identity", [&] {
        xfrontend::SourceManager sources;
        const auto core_id = sources.add_buffer("core.src", "module Core; public function Start() {} ");
        const auto app_id = sources.add_buffer("app.src", "module App; import Missing.Core; function Main() {} ");
        auto core = xfrontend::parse_source(sources, core_id);
        auto app = xfrontend::parse_source(sources, app_id);
        module_require(core.succeeded() && app.succeeded(), "sources should parse");
        ProgramFile core_file; core_file.display_name = "core.src"; core_file.source_id = core_id; core_file.unit = std::make_shared<xfrontend::CompilationUnit>(std::move(core.unit));
        ProgramFile app_file; app_file.display_name = "app.src"; app_file.source_id = app_id; app_file.unit = std::make_shared<xfrontend::CompilationUnit>(std::move(app.unit));
        const auto result = resolve_modules({std::move(core_file), std::move(app_file)});
        const auto found = std::find_if(result.diagnostics.begin(), result.diagnostics.end(),
            [](const auto& item) { return item.code == "XMOD0003"; });
        module_require(found != result.diagnostics.end(), "missing import diagnostic expected");
        module_require(found->span.source_id == app_id, "import diagnostic must retain source id");
    }, passed, failed);

    module_case("R027_program_semantics_private_cross_file_helper", [&] {
        const auto result = xfrontend::analyze_program({
            program_file("decl.src", "module Core; function Helper(int value) returns int;"),
            program_file("impl.src", "module Core; function Helper(int value) returns int { return value + 1; } function Main() returns int { return Helper(4); }")});
        module_require(result.succeeded(), "private same-module helper should resolve across files");
    }, passed, failed);

    module_case("R028_program_semantics_public_cross_module_call", [&] {
        const auto result = xfrontend::analyze_program({
            program_file("api.src", "module Api; public function Get() returns int { return 9; }"),
            program_file("app.src", "module App; import Api; function Main() returns int { return Api.Get(); }")});
        module_require(result.succeeded(), "public imported function should link at semantic program level");
    }, passed, failed);

    module_case("R029_program_semantics_private_cross_module_rejected", [&] {
        const auto result = xfrontend::analyze_program({
            program_file("api.src", "module Api; function Hidden() {}"),
            program_file("app.src", "module App; import Api; function Main() { Api.Hidden(); }")});
        module_require(has_semantic_code(result, "XMOD0006"), "private access must be rejected");
    }, passed, failed);

    module_case("R030_program_semantics_module_scope_is_casefolded", [&] {
        const auto result = xfrontend::analyze_program({
            program_file("a.src", "module Core; function Helper() {}"),
            program_file("b.src", "module core; function Main() { HELPER(); }")});
        module_require(result.succeeded(), "same-module unqualified lookup should ignore case");
    }, passed, failed);

    module_case("R031_source_manager_program_analysis", [&] {
        xfrontend::SourceManager sources;
        const auto api = sources.add_buffer("api.src", "module Api; public function Value() returns int { return 3; }");
        const auto app = sources.add_buffer("app.src", "module App; import Api; function Main() returns int { return Api.Value(); }");
        const auto result = xfrontend::analyze_program(sources, {api, app});
        module_require(result.succeeded(), "SourceManager multi-file program analysis should pass");
    }, passed, failed);

    module_case("R032_source_manager_program_parse_error_propagates", [&] {
        xfrontend::SourceManager sources;
        const auto id = sources.add_buffer("broken.src", "module Broken; function Bad( {");
        const auto result = xfrontend::analyze_program(sources, {id});
        module_require(!result.succeeded(), "parse error should fail program analysis");
        module_require(!result.diagnostics.empty() && result.diagnostics[0].code.find("XPARSE") == 0U,
                       "parser diagnostic should propagate");
        module_require(result.diagnostics[0].span.source_id == id, "parser source id should remain intact");
    }, passed, failed);

    module_case("R033_unknown_source_id_diagnostic", [&] {
        xfrontend::SourceManager sources;
        const auto result = xfrontend::analyze_program(sources, {999U});
        module_require(has_semantic_code(result, "XSRC0001"), "unknown source identity should be diagnosed");
    }, passed, failed);

    module_case("R034_duplicate_source_files_are_grouped_once", [&] {
        const auto result = resolve_modules({
            program_file("one.src", "module Shared; public function A() {}", 1U),
            program_file("two.src", "module Shared; public function B() {}", 2U)});
        module_require(result.succeeded(), "two source files should be accepted");
        module_require(result.modules.size() == 1U, "module grouping should be unique");
        module_require(result.modules[0].source_ids.size() == 2U, "both source ids should be listed");
    }, passed, failed);

    module_case("R035_private_overload_does_not_become_public", [&] {
        const auto result = resolve_modules({
            program_file("api.src", "module Api; public function Run(string text) {} function Run(int code) {}"),
            program_file("app.src", "module App; import Api; function Main() { Api.Run(4); }")});
        module_require(has_module_code(result, "XMOD0006"),
                       "matching private overload must not be exposed through another public overload");
    }, passed, failed);

    module_case("R036_imported_function_requires_qualified_name", [&] {
        const auto result = xfrontend::analyze_program({
            program_file("api.src", "module Api; public function Run() {}"),
            program_file("app.src", "module App; import Api; function Main() { Run(); }")});
        module_require(has_semantic_code(result, "XSEM0008"),
                       "import does not inject external functions into unqualified scope");
    }, passed, failed);

    module_case("R037_imported_overload_selects_known_argument_type", [&] {
        const auto result = resolve_modules({
            program_file("api.src", "module Api; public function Send(int code) {} public function Send(string text) {}"),
            program_file("app.src", "module App; import Api; function Main() { Api.Send(12); }")});
        module_require(result.succeeded(), "integer argument should select the integer public overload");
    }, passed, failed);

    module_case("R038_imported_overload_rejects_incompatible_type", [&] {
        const auto result = resolve_modules({
            program_file("api.src", "module Api; public function Send(int code) {}"),
            program_file("app.src", "module App; import Api; function Main() { Api.Send(true); }")});
        module_require(has_module_code(result, "XMOD0012"), "known incompatible argument type should fail");
    }, passed, failed);

    module_case("R039_generic_imported_function_accepts_typed_literal", [&] {
        const auto result = resolve_modules({
            program_file("api.src", "module Api; public function Echo[T](T value) {}"),
            program_file("app.src", "module App; import Api; function Main() { Api.Echo(12); }")});
        module_require(result.succeeded(), "generic type parameter should accept a statically typed argument");
    }, passed, failed);

    module_case("R040_imported_function_default_argument", [&] {
        const auto result = resolve_modules({
            program_file("api.src", "module Api; public function Send(int code, string note = \"ok\") {}"),
            program_file("app.src", "module App; import Api; function Main() { Api.Send(12); }")});
        module_require(result.succeeded(), "default argument should reduce required arity");
    }, passed, failed);

    module_case("R041_public_function_interface_contains_typed_parameters", [&] {
        const auto result = resolve_modules({program_file("api.src",
            "module Api; public function Read(borrow int id, string locale = \"en\") returns string { return locale; }")});
        module_require(result.succeeded(), "public API should resolve");
        module_require(result.modules.size() == 1U && result.modules[0].public_functions.size() == 1U,
                       "one exported function interface expected");
        const auto& api = result.modules[0].public_functions[0];
        module_require(api.name == "Read" && api.return_type == "string", "function name and return type metadata expected");
        module_require(api.parameters.size() == 2U, "both parameter interfaces expected");
        module_require(api.parameters[0].passing_mode == "borrow", "borrow mode must be exported");
        module_require(api.required_parameter_count == 1U && api.parameters[1].has_default,
                       "default argument must be represented in interface metadata");
    }, passed, failed);

    module_case("R042_private_function_is_absent_from_public_interface", [&] {
        const auto result = resolve_modules({program_file("api.src",
            "module Api; public function Visible() returns int { return 1; } function Hidden() returns int { return 2; }")});
        module_require(result.succeeded(), "module should resolve");
        module_require(result.modules[0].public_functions.size() == 1U &&
                       result.modules[0].public_functions[0].name == "Visible",
                       "private symbols must not leak into exported interface");
    }, passed, failed);

    module_case("R043_prototype_definition_return_type_mismatch", [&] {
        const auto result = resolve_modules({
            program_file("decl.src", "module Api; public function Read(int id) returns int;"),
            program_file("impl.src", "module Api; public function Read(int id) returns string { return \"x\"; }")});
        module_require(has_module_code(result, "XMOD0013"),
                       "prototype and definition return contracts must agree");
    }, passed, failed);

    module_case("R044_prototype_definition_passing_mode_mismatch", [&] {
        const auto result = resolve_modules({
            program_file("decl.src", "module Api; public function Read(borrow int id) returns int;"),
            program_file("impl.src", "module Api; public function Read(int id) returns int { return id; }")});
        module_require(has_module_code(result, "XMOD0013"),
                       "prototype and definition parameter passing modes must agree");
    }, passed, failed);

    module_case("R045_prototype_default_argument_is_preserved_in_export", [&] {
        const auto result = resolve_modules({
            program_file("decl.src", "module Api; public function Send(int code, string note = \"ok\") returns int;"),
            program_file("impl.src", "module Api; public function Send(int code, string note) returns int { return code; }")});
        module_require(result.succeeded(), "matching prototype and definition should resolve");
        module_require(result.modules[0].public_functions.size() == 1U &&
                       result.modules[0].public_functions[0].required_parameter_count == 1U,
                       "prototype default must remain part of the merged interface");
    }, passed, failed);

    module_case("R046_imported_return_type_is_checked_in_assignment", [&] {
        const auto result = xfrontend::analyze_program({
            program_file("api.src", "module Api; public function Read() returns int { return 1; }"),
            program_file("app.src", "module App; import Api; function Main() { string text = Api.Read(); }")});
        module_require(has_semantic_code(result, "XSEM0013"),
                       "an imported int result must not initialize a string local");
    }, passed, failed);

    module_case("R047_imported_argument_type_is_checked_from_local_symbol", [&] {
        const auto result = xfrontend::analyze_program({
            program_file("api.src", "module Api; public function Send(string text) {}"),
            program_file("app.src", "module App; import Api; function Main() { int code = 3; Api.Send(code); }")});
        module_require(has_semantic_code(result, "XSEM0035"),
                       "known local int argument must not match exported string parameter");
    }, passed, failed);

    module_case("R048_imported_overload_uses_local_variable_type", [&] {
        const auto result = xfrontend::analyze_program({
            program_file("api.src", "module Api; public function Select(int value) returns int { return value; } public function Select(string value) returns string { return value; }"),
            program_file("app.src", "module App; import Api; function Main() { string text = \"ok\"; int number = Api.Select(text); }")});
        module_require(has_semantic_code(result, "XSEM0013"),
                       "string overload must be selected and its string return type checked");
        module_require(!has_module_code(resolve_modules({
            program_file("api.src", "module Api; public function Select(int value) {} public function Select(string value) {}"),
            program_file("app.src", "module App; import Api; function Main() { string text = \"ok\"; Api.Select(text); }")}), "XMOD0009"),
            "unknown-to-resolver local expressions must defer ambiguity to semantic analysis");
    }, passed, failed);

    module_case("R049_generic_imported_return_type_is_substituted", [&] {
        const auto result = xfrontend::analyze_program({
            program_file("api.src", "module Api; public function Echo[T](T value) returns T { return value; }"),
            program_file("app.src", "module App; import Api; function Main() { int number = Api.Echo(12); string text = Api.Echo(\"hello\"); }")});
        module_require(result.succeeded(), "generic return T should specialize from each argument type");
    }, passed, failed);

    module_case("R050_imported_call_uses_prototype_default", [&] {
        const auto result = xfrontend::analyze_program({
            program_file("decl.src", "module Api; public function Send(int code, string note = \"ok\") returns int;"),
            program_file("impl.src", "module Api; public function Send(int code, string note) returns int { return code; }"),
            program_file("app.src", "module App; import Api; function Main() returns int { return Api.Send(7); }")});
        module_require(result.succeeded(), "imported call should use a default declared on the prototype");
    }, passed, failed);

    module_case("R051_inferred_return_type_flows_through_module_chain", [&] {
        const auto result = xfrontend::analyze_program({
            program_file("app.src", "module App; import Bridge; function Main() { string text = Bridge.Forward(); }"),
            program_file("bridge.src", "module Bridge; import Api; public function Forward() { return Api.Read(); }"),
            program_file("api.src", "module Api; public function Read() { return 12; }")});
        module_require(has_semantic_code(result, "XSEM0013"),
                       "inferred int return should propagate through the imported Bridge interface");
    }, passed, failed);

    module_case("R052_imported_return_type_is_available_for_arithmetic_checks", [&] {
        const auto result = xfrontend::analyze_program({
            program_file("api.src", "module Api; public function Text() returns string { return \"x\"; }"),
            program_file("app.src", "module App; import Api; function Main() { int total = Api.Text() + 1; }")});
        module_require(has_semantic_code(result, "XSEM0029"),
                       "imported string return must participate in arithmetic type checking");
    }, passed, failed);

    module_case("R053_module_interface_lookup_is_case_insensitive", [&] {
        const auto result = xfrontend::analyze_program({
            program_file("api.src", "module Vendor.Device; public function Read() returns int { return 1; }"),
            program_file("app.src", "module App; import vendor.device; function Main() { string text = vendor.device.Read(); }")});
        module_require(has_semantic_code(result, "XSEM0013"),
                       "module interface lookup must ignore ASCII case");
    }, passed, failed);

    module_case("R054_public_interface_keeps_generic_parameter_metadata", [&] {
        const auto result = resolve_modules({program_file("api.src",
            "module Api; public function Echo[T:Numeric](T value) returns T { return value; }")});
        module_require(result.succeeded(), "generic public function should resolve");
        module_require(result.modules[0].public_functions.size() == 1U &&
                       result.modules[0].public_functions[0].generic_parameters.size() == 1U &&
                       result.modules[0].public_functions[0].generic_parameters[0] == "T:Numeric",
                       "generic parameter and constraint must be carried into interface");
    }, passed, failed);

    module_case("R055_inferred_void_public_interface_is_exported", [&] {
        const auto result = xfrontend::analyze_program({
            program_file("api.src", "module Api; public function Notify() { PrintLine \"ready\"; }"),
            program_file("app.src", "module App; import Api; function Main() { Api.Notify(); }")});
        module_require(result.succeeded(), "inferred void imported function should be callable");
    }, passed, failed);

    module_case("R056_imported_numeric_promotion_is_supported", [&] {
        const auto result = xfrontend::analyze_program({
            program_file("api.src", "module Api; public function Scale(float value) returns float { return value; }"),
            program_file("app.src", "module App; import Api; function Main() { float value = Api.Scale(2); }")});
        module_require(result.succeeded(), "int argument should promote to an imported float parameter");
    }, passed, failed);


    module_case("R057_exported_prototype_without_definition_keeps_signature", [&] {
        const auto result = resolve_modules({program_file("api.src",
            "module Api; public function Lookup(int id) returns string;")});
        module_require(result.succeeded(), "public prototype is a valid module interface");
        module_require(result.modules[0].public_functions.size() == 1U &&
                       !result.modules[0].public_functions[0].has_definition &&
                       result.modules[0].public_functions[0].return_type == "string",
                       "prototype metadata should preserve declared return type and definition state");
    }, passed, failed);

    module_case("R058_imported_prototype_can_be_type_checked_without_body", [&] {
        const auto result = xfrontend::analyze_program({
            program_file("api.src", "module Api; public function Lookup(int id) returns string;"),
            program_file("app.src", "module App; import Api; function Main() { int id = 1; int count = Api.Lookup(id); }")});
        module_require(has_semantic_code(result, "XSEM0013"),
                       "prototype return type must be visible even before a body is linked");
    }, passed, failed);

    module_case("R059_imported_void_result_cannot_initialize_value", [&] {
        const auto result = xfrontend::analyze_program({
            program_file("api.src", "module Api; public function Notify() { PrintLine \"done\"; }"),
            program_file("app.src", "module App; import Api; function Main() { int result = Api.Notify(); }")});
        module_require(has_semantic_code(result, "XSEM0013"),
                       "void imported function must not be treated as an unknown value");
    }, passed, failed);

    module_case("R060_imported_overload_selected_from_typed_parameter", [&] {
        const auto result = xfrontend::analyze_program({
            program_file("api.src", "module Api; public function Pick(int value) returns int { return value; } public function Pick(string value) returns string { return value; }"),
            program_file("app.src", "module App; import Api; function Main(string input) { string output = Api.Pick(input); }")});
        module_require(result.succeeded(), "semantic analyzer should select string overload from parameter type");
    }, passed, failed);

    module_case("R061_generic_same_type_parameters_reject_mixed_arguments", [&] {
        const auto result = xfrontend::analyze_program({
            program_file("api.src", "module Api; public function Pair[T](T first, T second) returns T { return first; }"),
            program_file("app.src", "module App; import Api; function Main() { int value = Api.Pair(1, \"mixed\"); }")});
        module_require(has_semantic_code(result, "XSEM0035"),
                       "a repeated generic parameter must infer one compatible type");
    }, passed, failed);

    module_case("R062_imported_default_argument_preserves_required_count", [&] {
        const auto result = resolve_modules({
            program_file("decl.src", "module Api; public function Send(int code, string note = \"ok\") returns int;"),
            program_file("impl.src", "module Api; public function Send(int code, string note) returns int { return code; }")});
        module_require(result.succeeded(), "prototype-definition pair should be accepted");
        const auto& api = result.modules[0].public_functions[0];
        module_require(api.required_parameter_count == 1U && api.parameters.size() == 2U,
                       "merged exported signature must preserve required and total counts");
    }, passed, failed);

    module_case("R063_imported_call_result_can_feed_another_imported_call", [&] {
        const auto result = xfrontend::analyze_program({
            program_file("app.src", "module App; import Api; import Formatter; function Main() { string text = Formatter.Format(Api.Read()); }"),
            program_file("fmt.src", "module Formatter; public function Format(string value) returns string { return value; }"),
            program_file("api.src", "module Api; public function Read() returns string { return \"ready\"; }")});
        module_require(result.succeeded(), "imported return type should flow into a second imported call");
    }, passed, failed);

    module_case("R064_inferred_exported_return_type_is_used_cross_file", [&] {
        const auto result = xfrontend::analyze_program({
            program_file("impl.src", "module Api; public function Read() { return 21; }"),
            program_file("app.src", "module App; import Api; function Main() { string text = Api.Read(); }")});
        module_require(has_semantic_code(result, "XSEM0013"),
                       "return type inferred in a module body should be published to importers");
    }, passed, failed);

    module_case("R065_prototype_and_definition_generic_constraints_must_match", [&] {
        const auto result = resolve_modules({
            program_file("decl.src", "module Api; public function Echo[T:Numeric](T value) returns T;"),
            program_file("impl.src", "module Api; public function Echo[T:Comparable](T value) returns T { return value; }")});
        module_require(has_module_code(result, "XMOD0013"),
                       "generic constraints are part of the public function contract");
    }, passed, failed);

    module_case("R066_imported_return_type_preserves_module_source_identity", [&] {
        xfrontend::SourceManager sources;
        const auto api_id = sources.add_buffer("api.src",
            "module Api; public function Read() returns int { return 1; }");
        const auto app_id = sources.add_buffer("app.src",
            "module App; import Api; function Main() { string text = Api.Read(); }");
        const auto result = xfrontend::analyze_program(sources, {api_id, app_id});
        const auto found = std::find_if(result.diagnostics.begin(), result.diagnostics.end(),
            [](const auto& diagnostic) { return diagnostic.code == "XSEM0013"; });
        module_require(found != result.diagnostics.end(), "imported return mismatch diagnostic expected");
        module_require(found->span.source_id == app_id,
                       "diagnostic for consumer's imported call must identify the consumer file");
    }, passed, failed);

    module_case("R067_imported_generic_return_can_be_used_in_numeric_expression", [&] {
        const auto result = xfrontend::analyze_program({
            program_file("api.src", "module Api; public function Echo[T](T value) returns T { return value; }"),
            program_file("app.src", "module App; import Api; function Main() { int result = Api.Echo(2) + 3; }")});
        module_require(result.succeeded(), "generic return type should substitute into numeric expression");
    }, passed, failed);

    module_case("R068_interface_lookup_does_not_inject_unqualified_external_symbols", [&] {
        const auto result = xfrontend::analyze_program({
            program_file("api.src", "module Api; public function Read() returns int { return 1; }"),
            program_file("app.src", "module App; import Api; function Main() { int value = Read(); }")});
        module_require(has_semantic_code(result, "XSEM0008"),
                       "imported function remains qualified and is not injected into local scope");
    }, passed, failed);

    module_case("R069_module_interface_exported_signatures_match_function_records", [&] {
        const auto result = resolve_modules({program_file("api.src",
            "module Api; public function A() {} function Hidden() {} public function B(int value) {}")});
        module_require(result.succeeded(), "module should resolve");
        module_require(result.modules[0].public_function_signatures.size() ==
                       result.modules[0].public_functions.size() &&
                       result.modules[0].public_functions.size() == 2U,
                       "signature list and typed interface list must stay aligned");
    }, passed, failed);

    module_case("R070_imported_overload_rejects_boolean_for_numeric_parameter", [&] {
        const auto result = xfrontend::analyze_program({
            program_file("api.src", "module Api; public function Scale(float value) returns float { return value; }"),
            program_file("app.src", "module App; import Api; function Main() { bool flag = true; Api.Scale(flag); }")});
        module_require(has_semantic_code(result, "XSEM0035"),
                       "bool local must not be accepted as imported float parameter");
    }, passed, failed);

    module_case("R071_imported_function_interface_preserves_definition_flag", [&] {
        const auto result = resolve_modules({program_file("api.src",
            "module Api; public function Read() returns int { return 1; }")});
        module_require(result.succeeded(), "module should resolve");
        module_require(result.modules[0].public_functions[0].has_definition,
                       "implemented public function should be marked as defined");
    }, passed, failed);

}
