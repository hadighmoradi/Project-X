#include "xfrontend/drop_abi.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

namespace xfrontend {
namespace {

std::string fold_identifier(std::string value) {
    for (char& ch : value) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte >= static_cast<unsigned char>('A') && byte <= static_cast<unsigned char>('Z'))
            ch = static_cast<char>(byte - static_cast<unsigned char>('A') + static_cast<unsigned char>('a'));
    }
    return value;
}

std::string canonical_type(std::string value) {
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }), value.end());
    return fold_identifier(std::move(value));
}

std::string trim(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();
    if (first >= last) return {};
    return std::string(first, last);
}

std::string qualified_symbol_name(const DropCallableSymbol& symbol) {
    if (!trim(symbol.qualified_name).empty()) return trim(symbol.qualified_name);
    if (trim(symbol.module_name).empty()) return trim(symbol.name);
    if (trim(symbol.name).empty()) return {};
    return trim(symbol.module_name) + "." + trim(symbol.name);
}

void add_diagnostic(std::vector<Diagnostic>& diagnostics, const char* code,
                    const std::string& message, const SourceSpan& span = {}) {
    diagnostics.push_back(Diagnostic{code, message, span});
}

bool same_module(const std::string& left, const std::string& right) {
    return !trim(left).empty() && canonical_type(left) == canonical_type(right);
}

bool visible_to_caller(const DropCallableSymbol& symbol, const DropAbiResolutionOptions& options) {
    if (!symbol.accessible) return false;
    if (symbol.visibility == DropSymbolVisibility::Public) return true;
    return same_module(symbol.module_name, options.caller_module);
}

bool supported_convention(const DropCallableSymbol& symbol, DropInvocationKind kind,
                          const DropAbiResolutionOptions& options) {
    if (kind == DropInvocationKind::DestructorCall)
        return symbol.calling_convention == DropCallingConvention::XNative;
    if (kind != DropInvocationKind::CustomDeleterCall) return false;
    if (symbol.calling_convention == DropCallingConvention::XNative) return true;
    if (symbol.calling_convention == DropCallingConvention::C)
        return options.allow_c_abi_custom_deleter;
    return options.allow_system_abi_custom_deleter;
}

std::string convention_name(DropCallingConvention convention) {
    switch (convention) {
    case DropCallingConvention::XNative: return "xnative";
    case DropCallingConvention::C: return "c";
    case DropCallingConvention::System: return "system";
    }
    return "invalid";
}

std::string passing_name(DropParameterPassing passing) {
    switch (passing) {
    case DropParameterPassing::Value: return "value";
    case DropParameterPassing::Borrow: return "borrow";
    case DropParameterPassing::BorrowMut: return "borrow_mut";
    case DropParameterPassing::Take: return "take";
    }
    return "invalid";
}

std::string make_signature_key(const DropCallableSymbol& symbol) {
    std::ostringstream stream;
    stream << canonical_type(symbol.resource_type) << '|' << static_cast<int>(symbol.kind)
           << '|' << convention_name(symbol.calling_convention) << '|';
    if (symbol.kind == DropCallableKind::DestructorMethod)
        stream << canonical_type(symbol.receiver_type) << ':' << passing_name(symbol.receiver_passing);
    for (const auto& parameter : symbol.parameters)
        stream << '|' << canonical_type(parameter.type_name) << ':' << passing_name(parameter.passing)
               << ':' << (parameter.has_default ? 'd' : 'r');
    stream << "->" << canonical_type(symbol.return_type);
    return stream.str();
}

bool valid_symbol_shape(const DropCallableSymbol& symbol, DropInvocationKind invocation_kind) {
    if (trim(symbol.symbol_id).empty() || trim(symbol.name).empty() || trim(symbol.resource_type).empty())
        return false;
    if (symbol.generic || symbol.variadic || canonical_type(symbol.return_type) != "void") return false;
    if (symbol.failure_mode == DropFailureMode::MayFail) {
        // The failure policy was already fixed by N18. The symbol must agree with it.
    }
    if (invocation_kind == DropInvocationKind::DestructorCall) {
        if (symbol.kind != DropCallableKind::DestructorMethod ||
            canonical_type(symbol.receiver_type) != canonical_type(symbol.resource_type) ||
            symbol.receiver_passing != DropParameterPassing::BorrowMut ||
            !symbol.parameters.empty()) return false;
        return symbol.calling_convention == DropCallingConvention::XNative;
    }
    if (invocation_kind == DropInvocationKind::CustomDeleterCall) {
        if (symbol.kind == DropCallableKind::DestructorMethod || symbol.parameters.size() != 1U)
            return false;
        const auto& parameter = symbol.parameters.front();
        if (canonical_type(parameter.type_name) != canonical_type(symbol.resource_type) ||
            parameter.passing != DropParameterPassing::Take || parameter.has_default) return false;
        return true;
    }
    return false;
}

bool invocation_symbol_matches(const DropInvocation& invocation,
                               const DropCallableSymbol& symbol) {
    const std::string requested = canonical_type(invocation.routine_name);
    const std::string candidate = canonical_type(qualified_symbol_name(symbol));
    const std::string short_name = canonical_type(symbol.name);
    const bool name_match = requested == candidate || (requested.find('.') == std::string::npos && requested == short_name);
    return name_match && canonical_type(invocation.resource_type) == canonical_type(symbol.resource_type);
}

} // namespace

DropAbiBindingResult bind_drop_invocations_to_callables(
    const DropContractResolutionResult& resolved,
    const std::vector<DropCallableSymbol>& symbols,
    DropAbiResolutionOptions options) {
    DropAbiBindingResult result;
    if (!resolved.diagnostics.empty()) {
        result.diagnostics = resolved.diagnostics;
        return result;
    }

    for (const auto& function : resolved.functions) {
        DropAbiBindingFunction bound_function;
        bound_function.function_name = function.function_name;
        bound_function.variables = function.variables;
        bound_function.blocks.reserve(function.blocks.size());

        for (const auto& block : function.blocks) {
            DropAbiBindingBlock bound_block;
            bound_block.id = block.id;
            bound_block.reachable = block.reachable;
            if (!block.reachable) {
                bound_function.blocks.push_back(std::move(bound_block));
                continue;
            }

            for (const auto& invocation : block.invocations) {
                DropCallBinding binding;
                binding.invocation_kind = invocation.kind;
                binding.function_name = function.function_name;
                binding.owner_type = invocation.owner_type;
                binding.resource_type = invocation.resource_type;
                binding.routine_name = invocation.routine_name;
                binding.guard = invocation.guard;
                binding.failure_mode = invocation.failure_mode;
                binding.terminate_if_failed = invocation.terminate_if_failed;
                binding.block_id = invocation.block_id;
                binding.action_index = invocation.action_index;
                binding.variable_id = invocation.variable_id;
                binding.span = invocation.span;

                if (invocation.kind == DropInvocationKind::NoOp) {
                    binding.consumes_resource = false;
                    bound_block.bindings.push_back(std::move(binding));
                    continue;
                }

                std::vector<const DropCallableSymbol*> candidates;
                for (const auto& symbol : symbols) {
                    if (invocation_symbol_matches(invocation, symbol)) candidates.push_back(&symbol);
                }
                if (candidates.empty()) {
                    add_diagnostic(result.diagnostics, "XABI0001",
                                   "No callable symbol matches drop routine '" + invocation.routine_name +
                                       "' for resource type '" + invocation.resource_type + "'", invocation.span);
                    continue;
                }
                if (candidates.size() != 1U) {
                    add_diagnostic(result.diagnostics, "XABI0004",
                                   "Drop callable resolution is ambiguous for '" + invocation.routine_name + "'",
                                   invocation.span);
                    continue;
                }

                const DropCallableSymbol& symbol = *candidates.front();
                if (!symbol.resolved) {
                    add_diagnostic(result.diagnostics, "XABI0002",
                                   "Drop callable symbol is unresolved: '" + qualified_symbol_name(symbol) + "'",
                                   invocation.span);
                    continue;
                }
                if (!visible_to_caller(symbol, options)) {
                    add_diagnostic(result.diagnostics, "XABI0003",
                                   "Drop callable symbol is not visible from module '" + options.caller_module + "'",
                                   invocation.span);
                    continue;
                }
                if (symbol.failure_mode != invocation.failure_mode) {
                    add_diagnostic(result.diagnostics, "XABI0005",
                                   "Drop callable failure mode does not match the resolved drop contract",
                                   invocation.span);
                    continue;
                }
                if (!supported_convention(symbol, invocation.kind, options)) {
                    add_diagnostic(result.diagnostics, "XABI0006",
                                   "Calling convention is not supported for this drop callable kind",
                                   invocation.span);
                    continue;
                }
                if (symbol.linkage == DropSymbolLinkage::External && trim(symbol.linkage_name).empty()) {
                    add_diagnostic(result.diagnostics, "XABI0007",
                                   "External drop callable requires a linker-visible name", invocation.span);
                    continue;
                }
                if (symbol.linkage == DropSymbolLinkage::Internal && trim(symbol.symbol_id).empty()) {
                    add_diagnostic(result.diagnostics, "XABI0008",
                                   "Internal drop callable requires a stable symbol identity", invocation.span);
                    continue;
                }
                if (!valid_symbol_shape(symbol, invocation.kind)) {
                    add_diagnostic(result.diagnostics, "XABI0009",
                                   "Drop callable signature does not match the required destructor/deleter ABI",
                                   invocation.span);
                    continue;
                }

                binding.symbol_id = symbol.symbol_id;
                binding.linkage_name = symbol.linkage == DropSymbolLinkage::External
                    ? symbol.linkage_name : symbol.symbol_id;
                binding.signature_key = make_signature_key(symbol);
                binding.callable_kind = symbol.kind;
                binding.calling_convention = symbol.calling_convention;
                binding.visibility = symbol.visibility;
                binding.linkage = symbol.linkage;

                if (invocation.kind == DropInvocationKind::DestructorCall) {
                    DropAbiArgument argument;
                    argument.role = DropAbiArgumentRole::Receiver;
                    argument.operand = DropAbiOperandKind::ResourceAddress;
                    argument.parameter_index = 0U;
                    argument.type_name = invocation.resource_type;
                    argument.passing = DropParameterPassing::BorrowMut;
                    binding.arguments.push_back(std::move(argument));
                    binding.consumes_resource = false;
                } else {
                    DropAbiArgument argument;
                    argument.role = DropAbiArgumentRole::ExplicitParameter;
                    argument.operand = DropAbiOperandKind::OwnedResourceValue;
                    argument.parameter_index = 0U;
                    argument.type_name = invocation.resource_type;
                    argument.passing = DropParameterPassing::Take;
                    binding.arguments.push_back(std::move(argument));
                    binding.consumes_resource = true;
                }
                bound_block.bindings.push_back(std::move(binding));
            }
            bound_function.blocks.push_back(std::move(bound_block));
        }
        result.functions.push_back(std::move(bound_function));
    }

    if (!result.diagnostics.empty()) result.functions.clear();
    return result;
}

} // namespace xfrontend
