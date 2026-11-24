// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "xfrontend/target_abi.hpp"

#include <string>
#include <vector>

namespace xfrontend {

enum class NativeSymbolEntityKind { Function, Global, TypeDescriptor, DropThunk };

struct NativeSymbolParameter {
    std::string type_name;
    DropParameterPassing passing = DropParameterPassing::Value;
};

// A source-level declaration that is ready for native symbol identity assignment.
// The source frontend remains case-insensitive; mangled XNative symbols are stable
// across declaration order and do not depend on process-specific hash values.
struct NativeSymbolDeclaration {
    std::string symbol_id;
    std::string module_name;
    std::string source_name;
    std::string external_name;
    std::string return_type = "void";
    std::vector<NativeSymbolParameter> parameters;
    DropCallingConvention calling_convention = DropCallingConvention::XNative;
    DropSymbolVisibility visibility = DropSymbolVisibility::Public;
    DropSymbolLinkage linkage = DropSymbolLinkage::External;
    NativeSymbolEntityKind entity_kind = NativeSymbolEntityKind::Function;
    bool generic = false;
    bool variadic = false;
    bool defined = true;
};

struct NativeSymbolRecord {
    std::string symbol_id;
    std::string module_name;
    std::string canonical_source_name;
    std::string mangled_name;
    std::string overload_key;
    std::string signature_key;
    DropCallingConvention calling_convention = DropCallingConvention::XNative;
    DropSymbolVisibility visibility = DropSymbolVisibility::Public;
    DropSymbolLinkage linkage = DropSymbolLinkage::External;
    NativeSymbolEntityKind entity_kind = NativeSymbolEntityKind::Function;
    bool defined = true;
};

struct NativeSymbolTableResult {
    std::vector<NativeSymbolRecord> symbols;
    std::vector<Diagnostic> diagnostics;
    bool succeeded() const noexcept { return diagnostics.empty(); }
};

std::string canonicalize_native_symbol_name(const std::string& value);
std::string mangle_native_symbol(const NativeSymbolDeclaration& declaration,
                                 std::string* reason = nullptr);
NativeSymbolTableResult build_native_symbol_table(
    const std::vector<NativeSymbolDeclaration>& declarations);

struct NativeSymbolReference {
    std::string reference_id;
    std::string mangled_name;
    std::string referencing_module;
    std::string expected_signature_key;
    bool weak = false;
};

struct NativeSymbolResolution {
    std::string reference_id;
    std::string defining_symbol_id;
    std::string mangled_name;
    bool resolved = false;
};

struct NativeLinkResolutionResult {
    std::vector<NativeSymbolResolution> resolutions;
    std::vector<Diagnostic> diagnostics;
    bool succeeded() const noexcept { return diagnostics.empty(); }
};

NativeLinkResolutionResult resolve_native_symbol_references(
    const std::vector<NativeSymbolRecord>& definitions,
    const std::vector<NativeSymbolReference>& references);

struct NativeLinkedCall {
    std::string function_name;
    FlowBlockId block_id = InvalidFlowBlockId;
    std::size_t action_index = 0U;
    FlowVariableId variable_id = InvalidFlowVariableId;
    std::string symbol_id;
    std::string mangled_name;
    std::string target_triple;
    std::vector<TargetAbiArgumentPlacement> arguments;
};

struct NativeLinkPlanResult {
    std::vector<NativeLinkedCall> calls;
    std::vector<Diagnostic> diagnostics;
    bool succeeded() const noexcept { return diagnostics.empty(); }
};

// Connects target-specific call placement to stable native symbol identities.
// This creates a link plan only; it does not emit object files or invoke a linker.
NativeLinkPlanResult bind_target_abi_calls_to_native_symbols(
    const TargetAbiLoweringResult& target_plan,
    const NativeSymbolTableResult& symbol_table);

} // namespace xfrontend
