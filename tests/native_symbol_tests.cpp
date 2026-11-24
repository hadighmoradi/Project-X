// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/native_symbol.hpp"
#include "test_report.hpp"

#include <chrono>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace xfrontend;

void symbol_require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void symbol_case(const std::string& name, const std::function<void()>& body,
                 int& passed, int& failed) {
    if (!xfrontend_test_report::should_run(name)) return;
    const auto start = std::chrono::steady_clock::now();
    try {
        body();
        ++passed;
        xfrontend_test_report::record(name, true, "", std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
    } catch (const std::exception& error) {
        ++failed;
        xfrontend_test_report::record(name, false, error.what(), std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
    }
}

NativeSymbolDeclaration declaration(int index) {
    NativeSymbolDeclaration value;
    value.symbol_id = "sym" + std::to_string(index);
    value.module_name = "Core.Module" + std::to_string(index % 19);
    value.source_name = "Dispose" + std::to_string(index % 13);
    value.return_type = "void";
    value.parameters.push_back({"Resource" + std::to_string(index % 23),
                                static_cast<DropParameterPassing>(index % 4)});
    value.calling_convention = DropCallingConvention::XNative;
    value.visibility = DropSymbolVisibility::Public;
    value.linkage = DropSymbolLinkage::External;
    value.entity_kind = NativeSymbolEntityKind::Function;
    return value;
}

} // namespace

void run_native_symbol_tests(int& passed, int& failed) {
    // N21A: deterministic, case-insensitive and injective symbol identity matrix.
    for (int i = 0; i < 500; ++i) {
        symbol_case("N21A" + std::to_string(i + 1) + "_symbol_mangling_case" + std::to_string(i), [=] {
            auto base = declaration(i);
            std::string reason;
            const std::string encoded = mangle_native_symbol(base, &reason);
            symbol_require(!encoded.empty(), "valid XNative declaration was not mangled: " + reason);
            symbol_require(encoded == mangle_native_symbol(base), "mangling is not deterministic");

            auto case_variant = base;
            for (char& ch : case_variant.module_name) {
                if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
            }
            for (char& ch : case_variant.source_name) {
                if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
            }
            symbol_require(encoded == mangle_native_symbol(case_variant), "case folding changed XNative identity");

            auto overload = base;
            overload.symbol_id += "_overload";
            overload.parameters.front().type_name += "Variant";
            symbol_require(encoded != mangle_native_symbol(overload), "distinct parameter types collided");

            auto return_only = base;
            return_only.return_type = "u64";
            symbol_require(encoded == mangle_native_symbol(return_only), "return type must not affect overload symbol identity");

            auto table = build_native_symbol_table({base, overload});
            symbol_require(table.succeeded() && table.symbols.size() == 2U,
                           "valid parameter overload pair was not accepted");
            symbol_require(table.symbols[0].signature_key != table.symbols[1].signature_key,
                           "overload signature fingerprints must differ");
        }, passed, failed);
    }

    // N21B: symbol table collisions, source visibility and external-name policy.
    for (int i = 0; i < 500; ++i) {
        symbol_case("N21B" + std::to_string(i + 1) + "_symbol_table_matrix_case" + std::to_string(i), [=] {
            auto base = declaration(1000 + i);
            const int scenario = i % 10;
            if (scenario == 0) {
                auto result = build_native_symbol_table({base});
                symbol_require(result.succeeded() && result.symbols.size() == 1U, "single public XNative symbol rejected");
            } else if (scenario == 1) {
                auto overload = base;
                overload.symbol_id += "_overload";
                overload.parameters.front().type_name += "Other";
                auto result = build_native_symbol_table({base, overload});
                symbol_require(result.succeeded() && result.symbols.size() == 2U, "parameter overload was not preserved");
            } else if (scenario == 2) {
                auto duplicate = base;
                duplicate.symbol_id += "_duplicate";
                auto result = build_native_symbol_table({base, duplicate});
                symbol_require(!result.succeeded(), "duplicate overload identity was accepted");
            } else if (scenario == 3) {
                auto return_only = base;
                return_only.symbol_id += "_return";
                return_only.return_type = "u64";
                auto result = build_native_symbol_table({base, return_only});
                symbol_require(!result.succeeded(), "return-only overload was accepted");
            } else if (scenario == 4) {
                base.calling_convention = DropCallingConvention::C;
                base.external_name = "c_export_" + std::to_string(i);
                auto second = base;
                second.symbol_id += "_collision";
                second.parameters.front().type_name += "Overload";
                auto result = build_native_symbol_table({base, second});
                symbol_require(!result.succeeded(), "C ABI overload collision was accepted");
            } else if (scenario == 5) {
                base.visibility = DropSymbolVisibility::Private;
                base.linkage = DropSymbolLinkage::External;
                auto result = build_native_symbol_table({base});
                symbol_require(!result.succeeded(), "private external symbol was accepted");
            } else if (scenario == 6) {
                base.generic = true;
                auto result = build_native_symbol_table({base});
                symbol_require(!result.succeeded(), "unspecialized generic symbol was accepted");
            } else if (scenario == 7) {
                base.variadic = true;
                auto result = build_native_symbol_table({base});
                symbol_require(!result.succeeded(), "XNative variadic declaration was accepted");
            } else if (scenario == 8) {
                base.calling_convention = DropCallingConvention::C;
                base.external_name = "invalid.name." + std::to_string(i);
                auto result = build_native_symbol_table({base});
                symbol_require(!result.succeeded(), "invalid C external name was accepted");
            } else {
                base.calling_convention = DropCallingConvention::System;
                base.external_name = "system_export_" + std::to_string(i);
                auto result = build_native_symbol_table({base});
                symbol_require(result.succeeded() && result.symbols.size() == 1U,
                               "valid System linkage declaration was rejected");
                symbol_require(result.symbols.front().mangled_name == base.external_name,
                               "System external name must remain unchanged");
            }
        }, passed, failed);
    }

    // N21C: cross-module linker resolution, internal visibility and weak references.
    for (int i = 0; i < 500; ++i) {
        symbol_case("N21C" + std::to_string(i + 1) + "_link_resolution_matrix_case" + std::to_string(i), [=] {
            auto decl = declaration(2000 + i);
            auto table = build_native_symbol_table({decl});
            symbol_require(table.succeeded() && table.symbols.size() == 1U, "test definition did not enter symbol table");
            const auto& symbol = table.symbols.front();
            NativeSymbolReference reference;
            reference.reference_id = "ref" + std::to_string(i);
            reference.mangled_name = symbol.mangled_name;
            reference.referencing_module = decl.module_name;
            reference.expected_signature_key = symbol.signature_key;

            const int scenario = i % 7;
            if (scenario == 0) {
                auto result = resolve_native_symbol_references(table.symbols, {reference});
                symbol_require(result.succeeded() && result.resolutions.size() == 1U &&
                               result.resolutions.front().resolved, "external definition did not resolve");
            } else if (scenario == 1) {
                decl.visibility = DropSymbolVisibility::Internal;
                decl.linkage = DropSymbolLinkage::Internal;
                table = build_native_symbol_table({decl});
                reference.mangled_name = table.symbols.front().mangled_name;
                reference.expected_signature_key = table.symbols.front().signature_key;
                reference.referencing_module = decl.module_name;
                auto result = resolve_native_symbol_references(table.symbols, {reference});
                symbol_require(result.succeeded() && result.resolutions.front().resolved,
                               "same-module internal definition did not resolve");
            } else if (scenario == 2) {
                decl.visibility = DropSymbolVisibility::Internal;
                decl.linkage = DropSymbolLinkage::Internal;
                table = build_native_symbol_table({decl});
                reference.mangled_name = table.symbols.front().mangled_name;
                reference.expected_signature_key = table.symbols.front().signature_key;
                reference.referencing_module = "Other.Module";
                auto result = resolve_native_symbol_references(table.symbols, {reference});
                symbol_require(!result.succeeded(), "cross-module reference to internal symbol was resolved");
            } else if (scenario == 3) {
                reference.mangled_name = "__X_missing_" + std::to_string(i);
                reference.expected_signature_key.clear();
                auto result = resolve_native_symbol_references({}, {reference});
                symbol_require(!result.succeeded(), "unresolved strong reference was accepted");
            } else if (scenario == 4) {
                reference.mangled_name = "__X_weak_missing_" + std::to_string(i);
                reference.weak = true;
                auto result = resolve_native_symbol_references({}, {reference});
                symbol_require(result.succeeded() && result.resolutions.size() == 1U &&
                               !result.resolutions.front().resolved, "unresolved weak reference should remain optional");
            } else if (scenario == 5) {
                reference.expected_signature_key += "_mismatch";
                auto result = resolve_native_symbol_references(table.symbols, {reference});
                symbol_require(!result.succeeded(), "link signature mismatch was accepted");
            } else {
                auto duplicate = table.symbols.front();
                duplicate.symbol_id += "_duplicate_definition";
                std::vector<NativeSymbolRecord> definitions = table.symbols;
                definitions.push_back(duplicate);
                auto result = resolve_native_symbol_references(definitions, {reference});
                symbol_require(!result.succeeded(), "duplicate external definitions were accepted");
            }
        }, passed, failed);
    }

    // N21D: connect N20 target-ABI placements to the stable N21 symbol table.
    for (int i = 0; i < 500; ++i) {
        symbol_case("N21D" + std::to_string(i + 1) + "_link_abi_integration_case" + std::to_string(i), [=] {
            auto decl = declaration(3000 + i);
            auto table = build_native_symbol_table({decl});
            symbol_require(table.succeeded(), "native symbol table setup failed");

            TargetAbiLoweringResult target;
            TargetAbiFunctionPlan function;
            function.function_name = "Caller" + std::to_string(i);
            TargetAbiBlockPlan block;
            block.id = static_cast<FlowBlockId>(i + 1);
            block.reachable = true;
            TargetAbiCallPlan call;
            call.symbol_id = decl.symbol_id;
            call.linkage_name = table.symbols.front().mangled_name;
            call.target_triple = (i % 2 == 0) ? "x86_64-unknown-windows-msvc" : "x86_64-unknown-linux-gnu";
            call.action_index = static_cast<std::size_t>(i);
            call.variable_id = static_cast<FlowVariableId>(i + 1);
            TargetAbiArgumentPlacement argument;
            argument.source_argument_index = 0U;
            argument.source_type = "Resource";
            argument.passed_indirectly = true;
            argument.locations.push_back(TargetAbiLocation{TargetLocationKind::GeneralPurposeRegister, 0U, 0U, 8U, 8U});
            call.arguments.push_back(argument);

            const int scenario = i % 5;
            if (scenario == 1) {
                call.is_noop = true;
                call.symbol_id = "missing-noop";
            } else if (scenario == 2) {
                block.reachable = false;
                call.symbol_id = "missing-unreachable";
            } else if (scenario == 3) {
                call.symbol_id = "missing-symbol";
            } else if (scenario == 4) {
                call.linkage_name = "wrong_external_symbol";
            }
            block.calls.push_back(call);
            function.blocks.push_back(block);
            target.functions.push_back(function);

            auto result = bind_target_abi_calls_to_native_symbols(target, table);
            if (scenario == 0) {
                symbol_require(result.succeeded() && result.calls.size() == 1U, "valid target call did not bind to native symbol");
                symbol_require(result.calls.front().mangled_name == table.symbols.front().mangled_name,
                               "link plan changed the mangled target");
                symbol_require(result.calls.front().arguments.size() == 1U,
                               "target ABI argument placements were not preserved");
            } else if (scenario == 1 || scenario == 2) {
                symbol_require(result.succeeded() && result.calls.empty(), "no-op or unreachable target call should be skipped");
            } else {
                symbol_require(!result.succeeded(), "invalid target-to-symbol binding was accepted");
            }
        }, passed, failed);
    }
}
