// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "xfrontend/drop_contract.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace xfrontend {

enum class DropCallableKind {
    DestructorMethod,
    FreeFunction,
    StaticFunction
};

enum class DropCallingConvention {
    XNative,
    C,
    System
};

enum class DropSymbolVisibility {
    Private,
    Internal,
    Public
};

enum class DropSymbolLinkage {
    Internal,
    External
};

enum class DropParameterPassing {
    Value,
    Borrow,
    BorrowMut,
    Take
};

enum class DropAbiArgumentRole {
    Receiver,
    ExplicitParameter
};

enum class DropAbiOperandKind {
    ResourceAddress,
    OwnedResourceValue
};

struct DropCallableParameter {
    std::string type_name;
    DropParameterPassing passing = DropParameterPassing::Value;
    bool has_default = false;
};

// A resolved callable symbol supplied by semantic/module symbol discovery.
// This is still target-independent metadata; it contains no register or stack layout.
struct DropCallableSymbol {
    std::string symbol_id;
    std::string module_name;
    std::string name;
    std::string qualified_name;
    std::string linkage_name;
    std::string resource_type;
    DropCallableKind kind = DropCallableKind::DestructorMethod;
    DropCallingConvention calling_convention = DropCallingConvention::XNative;
    DropSymbolVisibility visibility = DropSymbolVisibility::Public;
    DropSymbolLinkage linkage = DropSymbolLinkage::Internal;
    bool resolved = true;
    bool accessible = true;
    bool generic = false;
    bool variadic = false;
    std::string receiver_type;
    DropParameterPassing receiver_passing = DropParameterPassing::BorrowMut;
    std::vector<DropCallableParameter> parameters;
    std::string return_type = "void";
    DropFailureMode failure_mode = DropFailureMode::NoThrow;
};

struct DropAbiResolutionOptions {
    std::string caller_module;
    bool allow_c_abi_custom_deleter = true;
    bool allow_system_abi_custom_deleter = true;
};

struct DropAbiArgument {
    DropAbiArgumentRole role = DropAbiArgumentRole::Receiver;
    DropAbiOperandKind operand = DropAbiOperandKind::ResourceAddress;
    std::size_t parameter_index = 0;
    std::string type_name;
    DropParameterPassing passing = DropParameterPassing::BorrowMut;
};

struct DropCallBinding {
    DropInvocationKind invocation_kind = DropInvocationKind::NoOp;
    std::string function_name;
    std::string owner_type;
    std::string resource_type;
    std::string routine_name;
    std::string symbol_id;
    std::string linkage_name;
    std::string signature_key;
    DropCallableKind callable_kind = DropCallableKind::DestructorMethod;
    DropCallingConvention calling_convention = DropCallingConvention::XNative;
    DropSymbolVisibility visibility = DropSymbolVisibility::Public;
    DropSymbolLinkage linkage = DropSymbolLinkage::Internal;
    OwnershipActionGuard guard = OwnershipActionGuard::Never;
    DropFailureMode failure_mode = DropFailureMode::NoThrow;
    bool terminate_if_failed = false;
    bool consumes_resource = false;
    FlowBlockId block_id = InvalidFlowBlockId;
    std::size_t action_index = 0;
    FlowVariableId variable_id = InvalidFlowVariableId;
    SourceSpan span;
    std::vector<DropAbiArgument> arguments;
};

struct DropAbiBindingBlock {
    FlowBlockId id = InvalidFlowBlockId;
    bool reachable = false;
    std::vector<DropCallBinding> bindings;
};

struct DropAbiBindingFunction {
    std::string function_name;
    std::vector<FlowVariable> variables;
    std::vector<DropAbiBindingBlock> blocks;
};

struct DropAbiBindingResult {
    std::vector<DropAbiBindingFunction> functions;
    std::vector<Diagnostic> diagnostics;

    bool succeeded() const noexcept { return diagnostics.empty(); }
};

// Binds N18's resource-specific drop invocations to discovered callable symbols
// and builds an abstract language-level argument plan. Destructor methods receive
// borrow mut T as a hidden receiver; free/static custom deleters consume take T as
// their sole explicit argument. Target-specific physical ABI lowering is deferred.
DropAbiBindingResult bind_drop_invocations_to_callables(
    const DropContractResolutionResult& resolved,
    const std::vector<DropCallableSymbol>& symbols,
    DropAbiResolutionOptions options = {});

} // namespace xfrontend
