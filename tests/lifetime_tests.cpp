// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/lifetime.hpp"
#include "xfrontend/semantic.hpp"
#include "xfrontend/source_manager.hpp"
#include "test_report.hpp"

#include <chrono>

#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>
#include <string>

namespace {

void life_require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

xfrontend::LifetimeResult lifetime_result(const std::string& source) {
    const auto parsed = xfrontend::parse_source(source);
    if (!parsed.succeeded()) {
        throw std::runtime_error("test source failed to parse: " +
            parsed.diagnostics.front().code + " " + parsed.diagnostics.front().message);
    }
    return xfrontend::analyze_lifetimes(parsed.unit);
}

bool has_lifetime_code(const xfrontend::LifetimeResult& result, const std::string& code) {
    for (const auto& item : result.diagnostics) {
        if (item.code == code) return true;
    }
    return false;
}

bool has_semantic_lifetime_code(const xfrontend::SemanticResult& result,
                                const std::string& code) {
    for (const auto& item : result.diagnostics) {
        if (item.code == code) return true;
    }
    return false;
}

void life_case(const char* name, const std::function<void()>& body,
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

void life_valid(const std::string& source) {
    const auto result = lifetime_result(source);
    life_require(result.succeeded(), "unexpected lifetime diagnostic: " +
        (result.diagnostics.empty() ? std::string() : result.diagnostics.front().message));
}

void life_invalid(const std::string& source, const std::string& code) {
    const auto result = lifetime_result(source);
    life_require(has_lifetime_code(result, code), "expected " + code + " diagnostic");
}

} // namespace

void run_lifetime_tests(int& passed, int& failed) {
    using namespace xfrontend;

    life_case("T001_empty_unit", [&] {
        const auto result = lifetime_result("");
        life_require(result.succeeded(), "empty unit should have no lifetime diagnostics");
    }, passed, failed);

    life_case("T002_borrow_parameter_can_be_used_locally", [&] {
        life_valid("function Inspect(borrow *Device device) { PrintLine device; }");
    }, passed, failed);

    life_case("T003_borrow_mut_parameter_can_be_used_locally", [&] {
        life_valid("function Update(borrow mut *Device device) { device.Start(); }");
    }, passed, failed);

    life_case("T004_return_borrow_parameter_is_rejected", [&] {
        life_invalid("function Escape(borrow *Device device) returns *Device { return device; }",
                     "XLIFE0001");
    }, passed, failed);

    life_case("T005_return_borrow_mut_parameter_is_rejected", [&] {
        life_invalid("function Escape(borrow mut *Device device) returns *Device { return device; }",
                     "XLIFE0001");
    }, passed, failed);

    life_case("T006_borrow_name_matching_is_case_insensitive", [&] {
        life_invalid("function Escape(borrow *Device DeviceRef) returns *Device { return deviceref; }",
                     "XLIFE0001");
    }, passed, failed);

    life_case("T007_implicit_local_alias_cannot_be_returned", [&] {
        life_invalid("function Escape(borrow *Device device) returns *Device { alias = device; return alias; }",
                     "XLIFE0001");
    }, passed, failed);

    life_case("T008_explicit_pointer_local_alias_cannot_be_returned", [&] {
        life_invalid("function Escape(borrow *Device device) returns *Device { *Device alias = device; return alias; }",
                     "XLIFE0001");
    }, passed, failed);

    life_case("T009_transitive_local_alias_is_tracked", [&] {
        life_invalid("function Escape(borrow *Device device) returns *Device { first = device; second = first; return second; }",
                     "XLIFE0001");
    }, passed, failed);

    life_case("T010_borrow_in_list_return_is_rejected", [&] {
        life_invalid("function Escape(borrow *Device device) returns List[*Device] { return [device]; }",
                     "XLIFE0001");
    }, passed, failed);

    life_case("T011_address_of_borrowed_dereference_is_rejected", [&] {
        life_invalid("function Escape(borrow *Device device) returns *Device { return &*device; }",
                     "XLIFE0001");
    }, passed, failed);

    life_case("T012_dereferenced_value_is_not_mistaken_for_pointer_escape", [&] {
        life_valid("function CopyValue(borrow *Device device) returns Device { return *device; }");
    }, passed, failed);

    life_case("T013_member_store_of_borrow_is_rejected", [&] {
        life_invalid("function Store(borrow *Device device) { holder.Reference = device; }",
                     "XLIFE0002");
    }, passed, failed);

    life_case("T014_index_store_of_borrow_is_rejected", [&] {
        life_invalid("function Store(borrow *Device device) { items[0] = device; }",
                     "XLIFE0002");
    }, passed, failed);

    life_case("T015_member_store_of_borrow_alias_is_rejected", [&] {
        life_invalid("function Store(borrow *Device device) { local = device; holder.Reference = local; }",
                     "XLIFE0002");
    }, passed, failed);

    life_case("T016_unrelated_member_store_is_allowed", [&] {
        life_valid("function Store(borrow *Device device) { holder.Reference = other; }");
    }, passed, failed);

    life_case("T017_take_parameter_is_not_a_borrow", [&] {
        life_valid("function Consume(take Unique[Device] device) returns Unique[Device] { return device; }");
    }, passed, failed);

    life_case("T018_raw_pointer_without_borrow_contract_is_not_tainted", [&] {
        life_valid("function Raw(*Device device) returns *Device { return device; }");
    }, passed, failed);

    life_case("T019_copy_pointer_parameter_is_not_a_borrow", [&] {
        life_valid("function ClonePtr(copy *Device device) returns *Device { return device; }");
    }, passed, failed);

    life_case("T020_borrow_return_in_nested_branch_is_rejected", [&] {
        life_invalid("function Escape(bool flag, borrow *Device device) returns *Device { if (flag) { return device; } return null; }",
                     "XLIFE0001");
    }, passed, failed);

    life_case("T021_borrow_aliases_are_isolated_per_function", [&] {
        life_valid("function First(borrow *Device device) { PrintLine device; } function Second(*Device device) returns *Device { return device; }");
    }, passed, failed);

    life_case("T022_borrow_parameter_in_persian_comment_is_ignored", [&] {
        life_valid(u8"function Inspect(borrow *Device device) { // بازگرداندن device ممنوع است\n PrintLine \"device\"; }");
    }, passed, failed);

    life_case("T023_semantic_entry_point_runs_lifetime_analysis", [&] {
        const auto parsed = parse_source("function Escape(borrow *Device device) returns *Device { return device; }");
        life_require(parsed.succeeded(), "source should parse");
        const auto result = analyze_semantics(parsed.unit);
        life_require(has_semantic_lifetime_code(result, "XLIFE0001"),
                     "semantic entry point should include lifetime diagnostics");
    }, passed, failed);

    life_case("T024_source_manager_preserves_borrow_diagnostic_source_id", [&] {
        SourceManager sources;
        const SourceFileId id = sources.add_buffer(
            "escape.src", "function Escape(borrow *Device device) returns *Device { return device; }");
        const auto result = analyze_lifetimes(sources, id);
        life_require(has_lifetime_code(result, "XLIFE0001"), "lifetime diagnostic expected");
        life_require(result.diagnostics.front().span.source_id == id,
                     "diagnostic must preserve source identity");
    }, passed, failed);

    life_case("T025_program_entry_point_runs_lifetime_analysis", [&] {
        auto parsed = parse_source(
            "module Devices; function Escape(borrow *Device device) returns *Device { return device; }");
        life_require(parsed.succeeded(), "source should parse");
        ProgramFile file;
        file.display_name = "devices.src";
        file.source_id = 95U;
        file.unit = std::make_shared<CompilationUnit>(std::move(parsed.unit));
        const auto result = analyze_program({file});
        life_require(has_semantic_lifetime_code(result, "XLIFE0001"),
                     "program entry point should include lifetime diagnostics");
    }, passed, failed);

    life_case("T026_prototype_without_body_has_no_escape_check", [&] {
        life_valid("function Escape(borrow *Device device) returns *Device;");
    }, passed, failed);

    life_case("T027_overwrite_borrow_alias_with_raw_pointer_clears_taint", [&] {
        life_valid("function Escape(borrow *Device device, *Device other) returns *Device { alias = device; alias = other; return alias; }");
    }, passed, failed);

    life_case("T028_overwrite_borrow_alias_before_member_store_is_allowed", [&] {
        life_valid("function Store(borrow *Device device, *Device other) { alias = device; alias = other; holder.Reference = alias; }");
    }, passed, failed);

    life_case("T029_transitive_alias_keeps_original_borrow_after_source_overwrite", [&] {
        life_invalid("function Escape(borrow *Device device, *Device other) returns *Device { first = device; second = first; first = other; return second; }", "XLIFE0001");
    }, passed, failed);

    life_case("T030_overwriting_final_alias_clears_transitive_borrow", [&] {
        life_valid("function Escape(borrow *Device device, *Device other) returns *Device { first = device; second = first; second = other; return second; }");
    }, passed, failed);

    life_case("T031_one_conditional_borrow_assignment_remains_tainted", [&] {
        life_invalid("function Escape(bool flag, borrow *Device device, *Device other) returns *Device { alias = other; if (flag) { alias = device; } return alias; }", "XLIFE0001");
    }, passed, failed);

    life_case("T032_both_conditional_overwrites_clear_borrow_taint", [&] {
        life_valid("function Escape(bool flag, borrow *Device device, *Device other) returns *Device { alias = device; if (flag) { alias = other; } else { alias = null; } return alias; }");
    }, passed, failed);

    life_case("T033_conditional_assignment_without_else_keeps_possible_borrow", [&] {
        life_invalid("function Escape(bool flag, borrow *Device device) returns *Device { alias = null; if (flag) { alias = device; } return alias; }", "XLIFE0001");
    }, passed, failed);

    life_case("T034_borrow_assignment_in_both_branches_is_tainted", [&] {
        life_invalid("function Escape(bool flag, borrow *Device device, *Device other) returns *Device { alias = other; if (flag) { alias = device; } else { alias = device; } return alias; }", "XLIFE0001");
    }, passed, failed);

    life_case("T035_nested_conditional_alias_is_merged", [&] {
        life_invalid("function Escape(bool first, bool second, borrow *Device device, *Device other) returns *Device { alias = other; if (first) { if (second) { alias = device; } } return alias; }", "XLIFE0001");
    }, passed, failed);

    life_case("T036_loop_created_alias_is_possible_after_loop", [&] {
        life_invalid("function Escape(bool flag, borrow *Device device, *Device other) returns *Device { alias = other; while (flag) { alias = device; } return alias; }", "XLIFE0001");
    }, passed, failed);

    life_case("T037_loop_may_not_run_so_existing_borrow_alias_remains", [&] {
        life_invalid("function Escape(bool flag, borrow *Device device, *Device other) returns *Device { alias = device; while (flag) { alias = other; } return alias; }", "XLIFE0001");
    }, passed, failed);

    life_case("T038_loop_overwrite_does_not_prove_borrow_is_cleared", [&] {
        life_invalid("function Escape(bool flag, borrow *Device device) returns *Device { alias = device; while (flag) { alias = null; } return alias; }", "XLIFE0001");
    }, passed, failed);

    life_case("T039_borrow_return_in_one_branch_is_reported", [&] {
        life_invalid("function Escape(bool flag, borrow *Device device, *Device other) returns *Device { if (flag) { return device; } else { return other; } }", "XLIFE0001");
    }, passed, failed);

    life_case("T040_statements_after_both_returning_branches_are_not_analyzed", [&] {
        life_valid("function Escape(bool flag, borrow *Device device, *Device other) returns *Device { if (flag) { return other; } else { return other; } return device; }");
    }, passed, failed);

    life_case("T041_statement_after_return_does_not_create_borrow_escape", [&] {
        life_valid("function Escape(borrow *Device device, *Device other) returns *Device { return other; alias = device; }");
    }, passed, failed);

    life_case("T042_nested_block_assignment_to_outer_alias_is_tracked", [&] {
        life_invalid("function Escape(borrow *Device device, *Device other) returns *Device { alias = other; { alias = device; } return alias; }", "XLIFE0001");
    }, passed, failed);

    life_case("T043_nested_block_overwrite_clears_outer_alias", [&] {
        life_valid("function Escape(borrow *Device device, *Device other) returns *Device { alias = device; { alias = other; } return alias; }");
    }, passed, failed);

    life_case("T044_both_conditional_paths_can_clear_alias", [&] {
        life_valid("function Escape(bool flag, borrow *Device device, *Device first, *Device second) returns *Device { alias = device; if (flag) { alias = first; } else { alias = second; } return alias; }");
    }, passed, failed);

    life_case("T045_reassigned_borrow_parameter_no_longer_carries_old_origin", [&] {
        life_valid("function Escape(borrow *Device device, *Device other) returns *Device { device = other; return device; }");
    }, passed, failed);

    life_case("T046_alias_reassignment_uses_case_insensitive_symbol", [&] {
        life_valid("function Escape(borrow *Device device, *Device other) returns *Device { Alias = device; aLIAS = other; return ALIAS; }");
    }, passed, failed);

    life_case("T047_same_pointer_to_borrow_mut_and_borrow_is_rejected", [&] {
        life_invalid("function Update(borrow mut *Device target, borrow *Device source) { } function Caller(*Device pointer) { Update(pointer, pointer); }", "XLIFE0003");
    }, passed, failed);

    life_case("T048_same_pointer_to_two_immutable_borrows_is_allowed", [&] {
        life_valid("function ReadBoth(borrow *Device first, borrow *Device second) { } function Caller(*Device pointer) { ReadBoth(pointer, pointer); }");
    }, passed, failed);

    life_case("T049_same_pointer_to_two_mutable_borrows_is_rejected", [&] {
        life_invalid("function UpdateBoth(borrow mut *Device first, borrow mut *Device second) { } function Caller(*Device pointer) { UpdateBoth(pointer, pointer); }", "XLIFE0003");
    }, passed, failed);

    life_case("T050_distinct_unique_owners_can_be_mutably_borrowed_together", [&] {
        life_valid("function Update(borrow mut *Device first, borrow *Device second) { } function Caller() { Unique[Device] a = Unique[Device].Create(); Unique[Device] b = Unique[Device].Create(); Update(a.Get(), b.Get()); }");
    }, passed, failed);

    life_case("T051_same_unique_owner_get_result_conflicts", [&] {
        life_invalid("function Update(borrow mut *Device first, borrow *Device second) { } function Caller() { Unique[Device] owner = Unique[Device].Create(); Update(owner.Get(), owner.Get()); }", "XLIFE0003");
    }, passed, failed);

    life_case("T052_alias_of_unique_get_conflicts_with_original_borrow", [&] {
        life_invalid("function Update(borrow mut *Device first, borrow *Device second) { } function Caller() { Unique[Device] owner = Unique[Device].Create(); *Device first = owner.Get(); *Device alias = first; Update(first, alias); }", "XLIFE0003");
    }, passed, failed);

    life_case("T053_reassigned_alias_to_distinct_owner_avoids_conflict", [&] {
        life_valid("function Update(borrow mut *Device first, borrow *Device second) { } function Caller() { Unique[Device] a = Unique[Device].Create(); Unique[Device] b = Unique[Device].Create(); alias = a.Get(); alias = b.Get(); Update(a.Get(), alias); }");
    }, passed, failed);

    life_case("T054_mutable_and_immutable_arguments_in_reverse_order_conflict", [&] {
        life_invalid("function Update(borrow *Device first, borrow mut *Device second) { } function Caller(borrow *Device pointer) { Update(pointer, &*pointer); }", "XLIFE0003");
    }, passed, failed);

    life_case("T055_call_conflict_is_found_when_mutable_parameter_is_second", [&] {
        life_invalid("function Update(borrow *Device first, borrow mut *Device second) { } function Caller(*Device pointer) { Update(pointer, pointer); }", "XLIFE0003");
    }, passed, failed);

    life_case("T056_omitted_default_borrow_argument_has_no_overlap", [&] {
        life_valid("function Update(borrow mut *Device first, borrow *Device second = null) { } function Caller(*Device pointer) { Update(pointer); }");
    }, passed, failed);

    life_case("T057_explicit_default_argument_participates_in_conflict_check", [&] {
        life_invalid("function Update(borrow mut *Device first, borrow *Device second = null) { } function Caller(*Device pointer) { Update(pointer, pointer); }", "XLIFE0003");
    }, passed, failed);

    life_case("T058_conflict_inside_conditional_expression_is_detected", [&] {
        life_invalid("function Ready(borrow mut *Device first, borrow *Device second) returns bool { return true; } function Caller(*Device pointer) { if (Ready(pointer, pointer)) { PrintLine true; } }", "XLIFE0003");
    }, passed, failed);

    life_case("T059_distinct_raw_pointer_parameters_may_alias", [&] {
        life_invalid("function Update(borrow mut *Device first, borrow *Device second) { } function Caller(*Device a, *Device b) { Update(a, b); }", "XLIFE0003");
    }, passed, failed);

    life_case("T060_local_alias_of_formal_pointer_conflicts", [&] {
        life_invalid("function Update(borrow mut *Device first, borrow *Device second) { } function Caller(*Device pointer) { alias = pointer; Update(pointer, alias); }", "XLIFE0003");
    }, passed, failed);

    life_case("T061_alias_created_on_one_branch_may_conflict", [&] {
        life_invalid("function Update(borrow mut *Device first, borrow *Device second) { } function Caller(bool flag, *Device a, *Device b) { alias = b; if (flag) { alias = a; } Update(a, alias); }", "XLIFE0003");
    }, passed, failed);

    life_case("T062_alias_overwritten_on_both_branches_can_be_disjoint", [&] {
        life_valid("function Update(borrow mut *Device first, borrow *Device second) { } function Caller(bool flag) { Unique[Device] a = Unique[Device].Create(); Unique[Device] b = Unique[Device].Create(); Unique[Device] c = Unique[Device].Create(); alias = a.Get(); if (flag) { alias = b.Get(); } else { alias = c.Get(); } Update(a.Get(), alias); }");
    }, passed, failed);

    life_case("T063_overload_conflict_is_deferred_when_not_shared_by_all_candidates", [&] {
        life_valid("function Update(borrow mut *Device first, borrow *Device second) { } function Update(borrow *Other first, borrow *Other second) { } function Caller(*Device pointer) { Update(pointer, pointer); }");
    }, passed, failed);

    life_case("T064_overload_conflict_shared_by_all_candidates_is_reported", [&] {
        life_invalid("function Update(borrow mut *Device first, borrow *Device second) { } function Update(borrow *Other first, borrow mut *Other second) { } function Caller(*Device pointer) { Update(pointer, pointer); }", "XLIFE0003");
    }, passed, failed);

    life_case("T065_member_call_is_not_mistaken_for_local_free_function", [&] {
        life_valid("function Caller(*Device pointer) { service.Update(pointer, pointer); }");
    }, passed, failed);

    life_case("T066_unknown_qualified_import_call_is_deferred", [&] {
        life_valid("function Caller(*Device pointer) { Devices.Update(pointer, pointer); }");
    }, passed, failed);

    life_case("T067_return_unique_get_is_rejected", [&] {
        life_invalid("function Escape() returns *Device { Unique[Device] owner = Unique[Device].Create(); return owner.Get(); }", "XLIFE0001");
    }, passed, failed);

    life_case("T068_member_store_of_unique_get_is_rejected", [&] {
        life_invalid("function Store() { Unique[Device] owner = Unique[Device].Create(); holder.Reference = owner.Get(); }", "XLIFE0002");
    }, passed, failed);

    life_case("T069_index_store_of_unique_get_is_rejected", [&] {
        life_invalid("function Store() { Unique[Device] owner = Unique[Device].Create(); items[0] = owner.Get(); }", "XLIFE0002");
    }, passed, failed);

    life_case("T070_unique_get_alias_is_valid_before_owner_reset", [&] {
        life_valid("function Inspect() { Unique[Device] owner = Unique[Device].Create(); *Device alias = owner.Get(); PrintLine alias; }");
    }, passed, failed);

    life_case("T071_unique_get_alias_use_after_reset_is_rejected", [&] {
        life_invalid("function Inspect() { Unique[Device] owner = Unique[Device].Create(); *Device alias = owner.Get(); owner.Reset(); PrintLine alias; }", "XLIFE0005");
    }, passed, failed);

    life_case("T072_unique_get_alias_use_after_take_is_rejected", [&] {
        life_invalid("function Inspect() { Unique[Device] owner = Unique[Device].Create(); *Device alias = owner.Get(); owner.Take(); PrintLine alias; }", "XLIFE0005");
    }, passed, failed);

    life_case("T073_unique_get_alias_use_after_release_is_rejected", [&] {
        life_invalid("function Inspect() { Unique[Device] owner = Unique[Device].Create(); *Device alias = owner.Get(); owner.Release(); PrintLine alias; }", "XLIFE0005");
    }, passed, failed);

    life_case("T074_transitive_unique_get_alias_is_invalidated", [&] {
        life_invalid("function Inspect() { Unique[Device] owner = Unique[Device].Create(); first = owner.Get(); second = first; owner.Reset(); PrintLine second; }", "XLIFE0005");
    }, passed, failed);

    life_case("T075_overwriting_stale_alias_with_other_owner_restores_validity", [&] {
        life_valid("function Inspect() { Unique[Device] a = Unique[Device].Create(); Unique[Device] b = Unique[Device].Create(); alias = a.Get(); a.Reset(); alias = b.Get(); PrintLine alias; }");
    }, passed, failed);

    life_case("T076_reinitializing_owner_does_not_revive_old_alias", [&] {
        life_invalid("function Inspect() { Unique[Device] owner = Unique[Device].Create(); oldAlias = owner.Get(); owner.Reset(); owner = Unique[Device].Create(); freshAlias = owner.Get(); PrintLine freshAlias; PrintLine oldAlias; }", "XLIFE0005");
    }, passed, failed);

    life_case("T077_reset_of_another_owner_does_not_invalidate_alias", [&] {
        life_valid("function Inspect() { Unique[Device] a = Unique[Device].Create(); Unique[Device] b = Unique[Device].Create(); alias = a.Get(); b.Reset(); PrintLine alias; }");
    }, passed, failed);

    life_case("T078_owner_method_matching_is_case_insensitive", [&] {
        life_invalid("function Inspect() { Unique[Device] owner = Unique[Device].Create(); alias = owner.Get(); owner.rEsEt(); PrintLine alias; }", "XLIFE0005");
    }, passed, failed);

    life_case("T079_conditional_owner_reset_may_invalidate_alias", [&] {
        life_invalid("function Inspect(bool flag) { Unique[Device] owner = Unique[Device].Create(); alias = owner.Get(); if (flag) { owner.Reset(); } PrintLine alias; }", "XLIFE0005");
    }, passed, failed);

    life_case("T080_only_fallthrough_branch_controls_post_if_lifetime", [&] {
        life_invalid("function Inspect(bool flag, *Device other) { Unique[Device] owner = Unique[Device].Create(); alias = owner.Get(); if (flag) { owner.Reset(); } else { return; } PrintLine alias; }", "XLIFE0005");
    }, passed, failed);

    life_case("T081_returning_invalidated_branch_does_not_poison_other_branch", [&] {
        life_valid("function Inspect(bool flag) { Unique[Device] owner = Unique[Device].Create(); alias = owner.Get(); if (flag) { owner.Reset(); return; } else { PrintLine alias; } PrintLine alias; }");
    }, passed, failed);

    life_case("T082_loop_may_invalidate_owner_borrow", [&] {
        life_invalid("function Inspect(bool flag) { Unique[Device] owner = Unique[Device].Create(); alias = owner.Get(); while (flag) { owner.Reset(); } PrintLine alias; }", "XLIFE0005");
    }, passed, failed);

    life_case("T083_owner_scope_exit_invalidates_escaped_local_alias", [&] {
        life_invalid("function Inspect() { *Device alias; { Unique[Device] owner = Unique[Device].Create(); alias = owner.Get(); } PrintLine alias; }", "XLIFE0005");
    }, passed, failed);

    life_case("T084_owner_from_outer_scope_remains_live_after_inner_block", [&] {
        life_valid("function Inspect() { Unique[Device] owner = Unique[Device].Create(); { alias = owner.Get(); PrintLine alias; } PrintLine owner; }");
    }, passed, failed);

    life_case("T085_get_from_default_empty_unique_has_no_resource_origin", [&] {
        life_valid("function Inspect() { Unique[Device] owner; *Device alias = owner.Get(); owner.Reset(); PrintLine alias; }");
    }, passed, failed);

    life_case("T086_get_from_untracked_raw_pointer_is_not_assumed_unique", [&] {
        life_valid("function Inspect(*Device owner) { alias = owner.Get(); owner.Reset(); PrintLine alias; }");
    }, passed, failed);

    life_case("T087_named_unique_owner_requires_explicit_take", [&] {
        life_invalid("function Consume(take Unique[Device] device) { } function Caller() { Unique[Device] owner = Unique[Device].Create(); Consume(owner); }", "XLIFE0006");
    }, passed, failed);

    life_case("T088_explicit_take_is_accepted_for_unique_parameter", [&] {
        life_valid("function Consume(take Unique[Device] device) { } function Caller() { Unique[Device] owner = Unique[Device].Create(); Consume(owner.Take()); }");
    }, passed, failed);

    life_case("T089_temporary_unique_value_can_be_passed_to_take", [&] {
        life_valid("function Consume(take Unique[Device] device) { } function Caller() { Consume(Unique[Device].Create()); }");
    }, passed, failed);

    life_case("T090_raw_pointer_take_parameter_does_not_require_unique_take", [&] {
        life_valid("function Consume(take *Device device) { } function Caller(*Device pointer) { Consume(pointer); }");
    }, passed, failed);

    life_case("T091_forwarding_take_unique_parameter_requires_explicit_transfer", [&] {
        life_invalid("function Consume(take Unique[Device] device) { } function Forward(take Unique[Device] source) { Consume(source); }", "XLIFE0006");
    }, passed, failed);

    life_case("T092_forwarding_take_unique_parameter_with_take_is_allowed", [&] {
        life_valid("function Consume(take Unique[Device] device) { } function Forward(take Unique[Device] source) { Consume(source.Take()); }");
    }, passed, failed);

    life_case("T093_get_alias_from_take_parameter_is_invalidated_by_take", [&] {
        life_invalid("function Inspect(take Unique[Device] owner) { alias = owner.Get(); owner.Take(); PrintLine alias; }", "XLIFE0005");
    }, passed, failed);

    life_case("T094_new_owner_generation_produces_fresh_valid_borrow", [&] {
        life_valid("function Inspect() { Unique[Device] owner = Unique[Device].Create(); oldAlias = owner.Get(); owner.Take(); owner = Unique[Device].Create(); freshAlias = owner.Get(); PrintLine freshAlias; }");
    }, passed, failed);

    life_case("T095_reset_then_recreate_allows_new_get_alias", [&] {
        life_valid("function Inspect() { Unique[Device] owner = Unique[Device].Create(); owner.Reset(); owner = Unique[Device].Create(); alias = owner.Get(); PrintLine alias; }");
    }, passed, failed);

    life_case("T096_owner_invalidation_diagnostic_preserves_source_id", [&] {
        SourceManager sources;
        const SourceFileId id = sources.add_buffer(
            "stale.src", "function Inspect() { Unique[Device] owner = Unique[Device].Create(); alias = owner.Get(); owner.Reset(); PrintLine alias; }");
        const auto result = analyze_lifetimes(sources, id);
        life_require(has_lifetime_code(result, "XLIFE0005"), "stale borrow diagnostic expected");
        life_require(result.diagnostics.front().span.source_id == id,
                     "diagnostic must preserve source identity");
    }, passed, failed);


    life_case("T097_immutable_borrow_cannot_be_upgraded_to_mut", [&] {
        life_invalid("function Update(borrow mut *Device target) { } function Caller(borrow *Device source) { Update(source); }", "XLIFE0003");
    }, passed, failed);

    life_case("T098_mutable_borrow_can_be_reborrowed_mutably", [&] {
        life_valid("function Update(borrow mut *Device target) { } function Caller(borrow mut *Device source) { Update(source); }");
    }, passed, failed);

    life_case("T099_live_immutable_alias_blocks_mutable_reborrow", [&] {
        life_invalid("function Update(borrow mut *Device target) { } function Caller(borrow mut *Device target, borrow *Device observer) { PrintLine observer; Update(target); PrintLine observer; }", "XLIFE0003");
    }, passed, failed);

    life_case("T100_dead_immutable_alias_does_not_block_mutable_reborrow", [&] {
        life_valid("function Update(borrow mut *Device target) { } function Caller(borrow mut *Device target, borrow *Device observer) { PrintLine observer; Update(target); }");
    }, passed, failed);

    life_case("T101_live_alias_of_mutable_borrow_blocks_other_mutable_reborrow", [&] {
        life_invalid("function Update(borrow mut *Device target) { } function Caller(borrow mut *Device target, borrow mut *Device source) { alias = source; Update(target); PrintLine alias; }", "XLIFE0003");
    }, passed, failed);

    life_case("T102_dead_local_alias_does_not_block_mutable_reborrow", [&] {
        life_valid("function Update(borrow mut *Device target) { } function Caller(borrow mut *Device target, borrow mut *Device source) { alias = source; PrintLine alias; Update(target); }");
    }, passed, failed);

    life_case("T103_live_unique_get_alias_blocks_mutable_call", [&] {
        life_invalid("function Update(borrow mut *Device target) { } function Caller() { Unique[Device] owner = Unique[Device].Create(); alias = owner.Get(); Update(owner.Get()); PrintLine alias; }", "XLIFE0003");
    }, passed, failed);

    life_case("T104_overwritten_alias_is_not_live_across_mutable_call", [&] {
        life_valid("function Update(borrow mut *Device target) { } function Caller() { Unique[Device] owner = Unique[Device].Create(); alias = owner.Get(); alias = null; Update(owner.Get()); }");
    }, passed, failed);

    life_case("T105_future_conditional_use_keeps_alias_live", [&] {
        life_invalid("function Update(borrow mut *Device target) { } function Caller(bool flag, borrow mut *Device target, borrow *Device observer) { Update(target); if (flag) { PrintLine observer; } }", "XLIFE0003");
    }, passed, failed);

    life_case("T106_unrelated_unique_owner_alias_does_not_conflict", [&] {
        life_valid("function Update(borrow mut *Device target) { } function Caller() { Unique[Device] first = Unique[Device].Create(); Unique[Device] second = Unique[Device].Create(); alias = first.Get(); Update(second.Get()); PrintLine alias; }");
    }, passed, failed);

    life_case("T107_dead_alias_of_mutable_borrow_allows_reborrow", [&] {
        life_valid("function Update(borrow mut *Device target) { } function Caller(borrow mut *Device target, borrow mut *Device source) { alias = source; PrintLine alias; Update(target); }");
    }, passed, failed);

    life_case("T108_multiple_mutable_arguments_detect_alias_conflict", [&] {
        life_invalid("function Update(borrow mut *Device first, borrow mut *Device second, borrow *Device observer) { } function Caller(borrow mut *Device value, borrow *Device observer) { Update(value, value, observer); }", "XLIFE0003");
    }, passed, failed);

}
